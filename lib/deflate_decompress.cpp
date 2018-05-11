/*
 * deflate_decompress.c - a decompressor for DEFLATE
 *
 * Originally public domain; changes after 2016-09-07 are copyrighted.
 *
 * Copyright 2016 Eric Biggers
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * ---------------------------------------------------------------------------
 *
 * This is a highly optimized DEFLATE decompressor.  When compiled with gcc on
 * x86_64, it decompresses data in about 52% of the time of zlib (48% if BMI2
 * instructions are available).  On other architectures it should still be
 * significantly faster than zlib, but the difference may be smaller.
 *
 * Why this is faster than zlib's implementation:
 *
 * - Word accesses rather than byte accesses when reading input
 * - Word accesses rather than byte accesses when copying matches
 * - Faster Huffman decoding combined with various DEFLATE-specific tricks
 * - Larger bitbuffer variable that doesn't need to be filled as often
 * - Other optimizations to remove unnecessary branches
 * - Only full-buffer decompression is supported, so the code doesn't need to
 *   support stopping and resuming decompression.
 * - On x86_64, compile a version of the decompression routine using BMI2
 *   instructions and use it automatically at runtime when supported.
 */

#include <pthread.h>

#include "assert.hpp"
#include "unistd.h"

#include "input_stream.hpp"
#include "decompressor.hpp"
#include "deflate_window.hpp"
#include "synchronizer.hpp"

#include "libdeflate.h"

/*****************************************************************************
 *                         Main decompression routine
 *****************************************************************************/

namespace {

template<typename OutWindow, typename might>
inline bool
do_uncompressed(InputStream& in_stream, OutWindow& out, const might&)
{
    /* Uncompressed block: copy 'len' bytes literally from the input
     * buffer to the output buffer.  */
    in_stream.align_input();

    if (unlikely(in_stream.available() < 4)) {
        PRINT_DEBUG("bad block, uncompressed check less than 4 bytes in input\n");
        return false;
    }

    u16 len = in_stream.pop_u16();
    u16 nlen = in_stream.pop_u16();

    if (might::fail_if(len != (u16)~nlen)) {
        PRINT_DEBUG("bad uncompressed block: len encoding check\n");
        return false;
    }

    if (unlikely(len > in_stream.available())) {
        PRINT_DEBUG("bad uncompressed block: len bigger than input stream \n");
        return false;
    }

    if (might::fail_if(!out.copy(in_stream, len))) {
        PRINT_DEBUG("bad uncompressed block: rejected by output window (non-ascii)\n");
        return false;
    };
    return true;
}

template<typename might>
static inline bool
prepare_dynamic(struct libdeflate_decompressor* restrict d, InputStream& in_stream, const might& might_tag)
{

    /* The order in which precode lengths are stored.  */
    static constexpr u8 deflate_precode_lens_permutation[DEFLATE_NUM_PRECODE_SYMS] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

    /* Read the codeword length counts.  */
    unsigned num_litlen_syms = in_stream.pop_bits(5) + 257;
    unsigned num_offset_syms = in_stream.pop_bits(5) + 1;
    const unsigned num_explicit_precode_lens = in_stream.pop_bits(4) + 4;

    /* Read the precode codeword lengths.  */
    in_stream.ensure_bits<DEFLATE_NUM_PRECODE_SYMS * 3>();

    for (unsigned i = 0; i < num_explicit_precode_lens; i++)
        d->u.precode_lens[deflate_precode_lens_permutation[i]] = in_stream.pop_bits(3);

    for (unsigned i = num_explicit_precode_lens; i < DEFLATE_NUM_PRECODE_SYMS; i++)
        d->u.precode_lens[deflate_precode_lens_permutation[i]] = 0;

    /* Build the decode table for the precode.  */
    if (might::fail_if(!build_precode_decode_table(d, might_tag)))
        return false;

    /* Expand the literal/length and offset codeword lengths.  */
    for (unsigned i = 0; i < num_litlen_syms + num_offset_syms;) {
        in_stream.ensure_bits<DEFLATE_MAX_PRE_CODEWORD_LEN + 7>();

        /* (The code below assumes that the precode decode table
         * does not have any subtables.)  */
        // static_assert(PRECODE_TABLEBITS == DEFLATE_MAX_PRE_CODEWORD_LEN);

        /* Read the next precode symbol.  */
        const u32 entry = d->u.l.precode_decode_table[in_stream.bits(DEFLATE_MAX_PRE_CODEWORD_LEN)];
        in_stream.remove_bits(entry & HUFFDEC_LENGTH_MASK);
        const unsigned presym = entry >> HUFFDEC_RESULT_SHIFT;

        if (presym < 16) {
            /* Explicit codeword length  */
            d->u.l.lens[i++] = presym;
            continue;
        }

        /* Run-length encoded codeword lengths  */

        /* Note: we don't need verify that the repeat count
         * doesn't overflow the number of elements, since we
         * have enough extra spaces to allow for the worst-case
         * overflow (138 zeroes when only 1 length was
         * remaining).
         *
         * In the case of the small repeat counts (presyms 16
         * and 17), it is fastest to always write the maximum
         * number of entries.  That gets rid of branches that
         * would otherwise be required.
         *
         * It is not just because of the numerical order that
         * our checks go in the order 'presym < 16', 'presym ==
         * 16', and 'presym == 17'.  For typical data this is
         * ordered from most frequent to least frequent case.
         */
        if (presym == 16) {
            /* Repeat the previous length 3 - 6 times  */
            if (might::fail_if(!(i != 0))) {
                PRINT_DEBUG("fail at (i!=0)\n");
                return false;
            }
            const u8 rep_val = d->u.l.lens[i - 1];
            const unsigned rep_count = 3 + in_stream.pop_bits(2);
            d->u.l.lens[i + 0] = rep_val;
            d->u.l.lens[i + 1] = rep_val;
            d->u.l.lens[i + 2] = rep_val;
            d->u.l.lens[i + 3] = rep_val;
            d->u.l.lens[i + 4] = rep_val;
            d->u.l.lens[i + 5] = rep_val;
            i += rep_count;
        } else if (presym == 17) {
            /* Repeat zero 3 - 10 times  */
            const unsigned rep_count = 3 + in_stream.pop_bits(3);
            d->u.l.lens[i + 0] = 0;
            d->u.l.lens[i + 1] = 0;
            d->u.l.lens[i + 2] = 0;
            d->u.l.lens[i + 3] = 0;
            d->u.l.lens[i + 4] = 0;
            d->u.l.lens[i + 5] = 0;
            d->u.l.lens[i + 6] = 0;
            d->u.l.lens[i + 7] = 0;
            d->u.l.lens[i + 8] = 0;
            d->u.l.lens[i + 9] = 0;
            i += rep_count;
        } else {
            /* Repeat zero 11 - 138 times  */
            const unsigned rep_count = 11 + in_stream.pop_bits(7);
            memset(&d->u.l.lens[i], 0, rep_count * sizeof(d->u.l.lens[i]));
            i += rep_count;
        }
    }

    if (!build_offset_decode_table(d, num_litlen_syms, num_offset_syms, might_tag)) {
        PRINT_DEBUG("fail at build_offset_decode_table(d, num_litlen_syms, num_offset_syms)\n");
        return false;
    }
    if (!build_litlen_decode_table(d, num_litlen_syms, num_offset_syms, might_tag)) {
        PRINT_DEBUG("fail at build_litlen_decode_table(d, num_litlen_syms, num_offset_syms)\n");
        return false;
    }

    return true;
}

enum class block_result : unsigned
{
    SUCCESS = 0,
    LAST_BLOCK = 1,
    WINDOW_OVERFLOW = 2,
    INVALID_BLOCK_TYPE,
    INVALID_DYNAMIC_HT,
    INVALID_UNCOMPRESSED_BLOCK,
    INVALID_LITERAL,
    INVALID_MATCH,
    TOO_MUCH_INPUT,
    NOT_ENOUGH_INPUT,
    INVALID_PARSE,
};

static constexpr const char* block_result_strings[] = {
    "SUCCESS",         "LAST_BLOCK",    "WINDOW_OVERFLOW", "INVALID_BLOCK_TYPE", "INVALID_DYNAMIC_HT", "INVALID_UNCOMPRESSED_BLOCK",
    "INVALID_LITERAL", "INVALID_MATCH", "TOO_MUCH_INPUT",  "NOT_ENOUGH_INPUT",   "INVALID_PARSE"
};

const char*
to_cstr(block_result result)
{
    return block_result_strings[static_cast<unsigned>(result)];
}

/* return true if block decompression went smoothly, false if not (probably due to corrupt data) */
template<typename OutWindow, typename might = ShouldSucceed>
inline block_result
do_block(struct libdeflate_decompressor* restrict main_d, InputStream& in_stream, OutWindow& out, const might& might_tag = {})
{
    /* Starting to read the next block.  */
    if (unlikely(!in_stream.ensure_bits<1 + 2 + 5 + 5 + 4>()))
        return block_result::NOT_ENOUGH_INPUT;

    /* BFINAL: 1 bit  */
    block_result success = in_stream.pop_bits(1) ? block_result::LAST_BLOCK : block_result::SUCCESS;

    /* BTYPE: 2 bits  */
    libdeflate_decompressor* restrict cur_d;
    switch (in_stream.pop_bits(2)) {
        case DEFLATE_BLOCKTYPE_DYNAMIC_HUFFMAN:
            if (might::fail_if(!prepare_dynamic(main_d, in_stream, might_tag)))
                return block_result::INVALID_DYNAMIC_HT;
            cur_d = main_d;
            break;

        case DEFLATE_BLOCKTYPE_UNCOMPRESSED:
            if (might::fail_if(!do_uncompressed(in_stream, out, might_tag)))
                return block_result::INVALID_UNCOMPRESSED_BLOCK;

            return might::succeed_if(out.notify_end_block(in_stream)) ? success : block_result::INVALID_PARSE;

        case DEFLATE_BLOCKTYPE_STATIC_HUFFMAN:
            cur_d = main_d->static_decompressor;
            break;

        default:
            return block_result::INVALID_BLOCK_TYPE;
    }

    /* Decompressing a Huffman block (either dynamic or static)  */
    DEBUG_FIRST_BLOCK(fprintf(stderr, "trying to decode huffman block\n");)

    /* The main DEFLATE decode loop  */
    for (;;) {
        /* Decode a litlen symbol.  */
        in_stream.ensure_bits<DEFLATE_MAX_LITLEN_CODEWORD_LEN>();
        // FIXME: entry should be const
        u32 entry = cur_d->u.litlen_decode_table[in_stream.bits(LITLEN_TABLEBITS)];
        if (entry & HUFFDEC_SUBTABLE_POINTER) {
            /* Litlen subtable required (uncommon case)  */
            in_stream.remove_bits(LITLEN_TABLEBITS);
            entry = cur_d->u.litlen_decode_table[((entry >> HUFFDEC_RESULT_SHIFT) & 0xFFFF) + in_stream.bits(entry & HUFFDEC_LENGTH_MASK)];
        }
        in_stream.remove_bits(entry & HUFFDEC_LENGTH_MASK);
        // PRINT_DEBUG("in_stream position %x\n",in_stream.in_next);
        if (entry & HUFFDEC_LITERAL) {
            /* Literal  */
            if (unlikely(out.available() == 0)) {
                if (might::fail_if(out.flush() == 0))
                    return block_result::WINDOW_OVERFLOW;
            }

            if (might::fail_if(!out.push(byte(entry >> HUFFDEC_RESULT_SHIFT)))) {
                return block_result::INVALID_LITERAL;
            }

            // fprintf(stderr,"literal: %c\n",byte(entry >> HUFFDEC_RESULT_SHIFT)); // this is indeed the plaintext decoded character, good to know
            continue;
        }

        /* Match or end-of-block  */
        entry >>= HUFFDEC_RESULT_SHIFT;
        in_stream.ensure_bits<in_stream.bitbuf_max_ensure>();

        /* Pop the extra length bits and add them to the length base to
         * produce the full length.  */
        const u32 length = (entry >> HUFFDEC_LENGTH_BASE_SHIFT) + in_stream.pop_bits(entry & HUFFDEC_EXTRA_LENGTH_BITS_MASK);

        /* The match destination must not end after the end of the
         * output buffer.  For efficiency, combine this check with the
         * end-of-block check.  We're using 0 for the special
         * end-of-block length, so subtract 1 and it turn it into
         * SIZE_MAX.  */
        // static_assert(HUFFDEC_END_OF_BLOCK_LENGTH == 0);
        if (unlikely(length - 1 >= out.available())) {
            if (likely(length == HUFFDEC_END_OF_BLOCK_LENGTH)) { // Block done
                return might::succeed_if(out.notify_end_block(in_stream)) ? success : block_result::INVALID_PARSE;
            } else { // Needs flushing
                if (unlikely(out.flush() == 0)) {
                    return block_result::WINDOW_OVERFLOW;
                }
                assert(length <= out.available());
            }
        }
        assert(length > 0); // length == 0 => EOB case should be handled here

        // if we end up here, it means we're at a match

        /* Decode the match offset.  */
        entry = cur_d->offset_decode_table[in_stream.bits(OFFSET_TABLEBITS)];
        if (entry & HUFFDEC_SUBTABLE_POINTER) {
            /* Offset subtable required (uncommon case)  */
            in_stream.remove_bits(OFFSET_TABLEBITS);
            entry = cur_d->offset_decode_table[((entry >> HUFFDEC_RESULT_SHIFT) & 0xFFFF) + +in_stream.bits(entry & HUFFDEC_LENGTH_MASK)];
        }
        in_stream.remove_bits(entry & HUFFDEC_LENGTH_MASK);
        entry >>= HUFFDEC_RESULT_SHIFT;

        /* Pop the extra offset bits and add them to the offset base to
         * produce the full offset.  */
        const u32 offset = (entry & HUFFDEC_OFFSET_BASE_MASK) + in_stream.pop_bits(entry >> HUFFDEC_EXTRA_OFFSET_BITS_SHIFT);

        /* Copy the match: 'length' bytes at 'out_next - offset' to
         * 'out_next'.  */
        if (might::fail_if(!out.copy_match(length, offset))) {
            return block_result::INVALID_MATCH;
        }
    }
}

#include <sys/mman.h>
void madvise_huge(const void* ptr, size_t n, int line = __builtin_LINE()) {
    size_t twomegminus1 = (2UL<<20)-1;

    auto iptr_start = reinterpret_cast<intptr_t>(ptr);
    iptr_start = (iptr_start + twomegminus1) & ~twomegminus1;

    auto iptr_stop = (iptr_start + n) & ~twomegminus1;


    fprintf(stderr, "%d: madvise_huge(%p, 0x%lx) => madvise(%p, 0x%lx, HUGEPAGE)=%d\n",
            line, ptr, n,
            reinterpret_cast<void*>(iptr_start), iptr_stop - iptr_start,
            madvise(reinterpret_cast<void*>(iptr_start), iptr_stop - iptr_start, /*MADV_HUGEPAGE*/ 14));
}

// FIXME: split this function such that InputStreams are backuped on the stack instead of manually
template<typename Window>
inline std::pair<Window, size_t>
do_skip(struct libdeflate_decompressor* restrict d,
        InputStream& in_stream,
        const size_t skip = 0,
        const unsigned nb_valid_blocks_confirm = 8,
        const size_t max_bits_skip = size_t(1) << (3 + 20), // 1MiB
        const size_t min_block_size = 1 << 13               // 8KiB
)
{
    if (skip == 0)
        return { Window(), size_t(0) };
    in_stream.skip(skip);

    SymbolicDummyContext<Window> out_window;
    madvise_huge(out_window.buffer, out_window.buffer_size*sizeof(typename Window::char_t));

    using char_t = typename Window::char_t;

    size_t bits_skipped = 0;
    for (; bits_skipped < max_bits_skip && in_stream.ensure_bits<1>(); bits_skipped++, in_stream.remove_bits(1)) {
        if (in_stream.bits(1)) // We don't except to find a final block
            continue;

        InputStream cur_in = in_stream;

        //        if(cur_in.position_bits() >= 415018) {
        //            fprintf(stderr, "Block reached!!\n");
        //        })

        block_result res = do_block(d, cur_in, out_window, ShouldFail{});

        if (unlikely(res == block_result::SUCCESS) && out_window.size() - out_window.context_size >= min_block_size) {

            size_t first_block_pos = in_stream.position_bits();
            fprintf(stderr, "Candidate block start at %lubits\n", first_block_pos);

            // Now we try to fill the window from this position untill overflow:
            char_t* backup_next = out_window.next;
            InputStream backup_in = cur_in;
            for (unsigned trial = 0; trial < nb_valid_blocks_confirm || res != block_result::SUCCESS;
                 trial++, backup_next = out_window.next, backup_in = cur_in) {
                res = do_block(d, cur_in, out_window, ShouldSucceed{});
            }

            if ((res == block_result::LAST_BLOCK) != (cur_in.available() == 0))
                res = res == block_result::LAST_BLOCK ? block_result::TOO_MUCH_INPUT : block_result::NOT_ENOUGH_INPUT;

            if (res <= block_result::WINDOW_OVERFLOW) {
                if (res == block_result::WINDOW_OVERFLOW) {
                    // Restore window and input stream before the last decoded block
                    out_window.next = backup_next;
                    in_stream = backup_in;
                } else { // otherwise, yield the input_stream after the last decoded block
                    in_stream = cur_in;
                }

                return { std::move(out_window), first_block_pos };
            } else {
                fprintf(stderr,
                        "False positive sync: (code %s)\n"
                        "\tin_stream position: %lu\n"
                        "\twindows size: %lu\n",
                        to_cstr(res),
                        cur_in.position_bits(),
                        out_window.size() - out_window.context_size);
            }
        }

        out_window.clear();
    }

    fprintf(stderr,
            "Failled to do %lu bytes skip:\n"
            "\tbits skipped:\t\t%lu/%lu\n"
            "\tinput remaning bytes:\t%lu\n",
            skip,
            bits_skipped,
            max_bits_skip,
            in_stream.available());
    exit(1);
}

template<typename F>
inline void
print_block_boundaries(struct libdeflate_decompressor* restrict d,
                       const InputStream& in_stream,
                       F&& on_boundary = [](InputStream strm) {},
                       size_t nb_blocks = ~size_t(0))
{
    SymbolicDummyContext<> out_window;
    InputStream cur_in = in_stream;
    for (size_t i = 0; i < nb_blocks; i++) {
        on_boundary(InputStream(cur_in));
        block_result res = do_block(d, cur_in, out_window, ShouldSucceed{});
        fprintf(stderr, "Block decompressed size: %ld\n", out_window.size() - out_window.context_size);
        if (unlikely(res == block_result::LAST_BLOCK))
            break;
        if (unlikely(res != block_result::SUCCESS)) {
            fprintf(stderr, "Error: %s\n", to_cstr(res));
            std::abort();
        }
        out_window.clear();
    }
    on_boundary(InputStream(cur_in));
}

template<typename Window>
inline block_result
decompress_loop(struct libdeflate_decompressor* restrict d, InputStream& in_stream, Window& window, synchronizer* stop)
{
    for (;;) {
        block_result res = do_block(d, in_stream, window, ShouldSucceed{});
        if (unlikely(res != block_result::SUCCESS)) {
            if (res == block_result::WINDOW_OVERFLOW || res == block_result::LAST_BLOCK)
                return res;

            fprintf(stderr, "Block error: %s\n", to_cstr(res));
            std::abort();
        }
        if (unlikely(stop && stop->caught_up_block(in_stream.position_bits())))
            return res;
    }
}

//template<typename Window> __attribute__((hot))
//inline void translate_with_context(const char*restrict context, const typename Window::char_t *restrict in, char *restrict out, size_t n) {
//    for (unsigned i = 0; i < n; i++) {
//        typename Window::char_t val = in[i];
//        if (likely(val <= Window::max_value)) {
//            out[i] = char(val);
//        } else {
//            unsigned backref_pos = val - (Window::max_value + 1);
//            assert(backref_pos < (1U << 15));
//            out[i] = context[backref_pos];
//        }
//    }
//}


//template<typename Window> __attribute__((hot))
//inline void translate_with_context(const char*restrict context, const typename Window::char_t *restrict in, char *restrict out, size_t n) {
//    const typename Window::char_t* end = in + n;
//    do {
//        *out++ = context[*in++];
//    } while(in < end);
//}


template<typename Window> __attribute__((hot))
inline void translate_with_context(const char*restrict context, const typename Window::char_t *restrict in, char *restrict out, size_t n) {
    const typename Window::char_t* end = in + n;
    for(size_t i = 0 ; i < n ; i++) {
        out[i] = context[in[i]];
    }
}

template<typename Window>
inline std::unique_ptr<char[]> make_context_lkt(const Window& window, const std::unique_ptr<char[]>& prev_ctx={}) {
    auto context_size = window.context_size;
    auto max_value = window.max_value;

    std::unique_ptr<char[]> context_lkt(new char[max_value + context_size]);
    for(unsigned i = 0; i <= max_value ; i++)
        context_lkt[i] = char(i);

    if(prev_ctx) {
        translate_with_context<Window>(prev_ctx.get(),
                               window.next - context_size,
                               context_lkt.get() + max_value + 1,
                               context_size);
    } else {
        assert(sizeof(typename Window::char_t)==1);
        memcpy(context_lkt.get() + max_value + 1, window.next - context_size, context_size);
    }
    return context_lkt;
}

static constexpr bool benchmark = true;

void
decompress_chunks(struct libdeflate_decompressor* restrict d,
                  InputStream in_stream,
                  size_t skip,
                  synchronizer* stop,     // indicating where to stop
                  synchronizer& prev_sync // for passing our first extracted sequence coordinate to the previous thread
)
{
    using backref_char_t = uint16_t;
    using Window = AsciiOnly<NoFlush<DeflateWindow<backref_char_t, 31>>>;

    auto pair = do_skip<Window>(d, in_stream, skip);
    Window& window = pair.first;
    size_t first_block_bit_pos = pair.second;

    prev_sync.signal_first_decoded_sequence(first_block_bit_pos, 0);
    fprintf(stderr, "Thread %lu synced at %lubits\n", skip, first_block_bit_pos);

    // No need to do more block since do_skip filled the window:
    if(decompress_loop(d, in_stream, window, stop) == block_result::WINDOW_OVERFLOW) {
        if(stop) {
            fprintf(stderr, "File too big to be decompressed !\n");
            abort();
        } else {
            fprintf(stderr, "Warning: File too big to be fully decompressed\n");
        }
    }

    fprintf(stderr, "Thread %lu ended at %lubits\n", skip, in_stream.position_bits());

    synchronizer::context_ptr context = prev_sync.get_context();
    fprintf(stderr, "Thread %lu got context\n", skip);

    // First, translate our context to pass it to the next thread ASAP
    if(stop) {
        stop->post_context(make_context_lkt(window, context));
    }

    if(!benchmark)
        prev_sync.wait_output();

    static constexpr Window::wsize_t buffer_size = 1 << 20;
    synchronizer::context_ptr buffer(new char[buffer_size]);
    for(Window::char_t* p = window.buffer + window.context_size ; p < window.next ; p+=buffer_size) {
        auto n = std::min(window.next - p, ssize_t(buffer_size));
        translate_with_context<Window>(context.get(), p, buffer.get(), n);
        if(!benchmark)
        if(write(1, buffer.get(), n) != n) {
            fprintf(stderr, "write error\n");
            std::abort();
        }
    }

    if(!benchmark && stop)
        stop->signal_output();
}

void
decompress_first_chunk(struct libdeflate_decompressor* restrict d, InputStream in_stream, synchronizer* stop)
{
    using char_t = unsigned char;
    using Window = AsciiOnly<NoFlush<DeflateWindow<char_t, 31>>>;

    Window window;
    madvise_huge(window.buffer, window.buffer_size*sizeof(typename Window::char_t));

    if(decompress_loop(d, in_stream, window, stop) == block_result::WINDOW_OVERFLOW) {
        fprintf(stderr, "File too big to be decompressed ! (first block)\n");
        abort();
    }

    fprintf(stderr, "Thread 0 ended at %lubits\n", in_stream.position_bits());

    if (stop) {
        stop->post_context(make_context_lkt(window));
    }

    if(!benchmark)
    if(write(1, window.buffer, window.size()) != ssize_t(window.size())) {
        fprintf(stderr, "write error");
        abort();
    }

    if(!benchmark && stop)
        stop->signal_output();
}

} /* namespace */

// Original API:

LIBDEFLATEAPI enum libdeflate_result
libdeflate_deflate_decompress(struct libdeflate_decompressor* restrict d,
                              const byte* restrict const in,
                              size_t in_nbytes,
                              byte* restrict const out,
                              size_t out_nbytes_avail,
                              size_t* actual_out_nbytes_ret,
                              synchronizer* stop,      // indicating where to stop
                              synchronizer* prev_sync, // for passing our first extracted sequence coordinate to the previous thread
                              size_t skip,
                              size_t until)
{
    fprintf(stderr, "madvise=%d\n", madvise((void*)((intptr_t)(in+4095) & ~intptr_t(4095)), in_nbytes & ~intptr_t(4095), MADV_SEQUENTIAL));
    madvise_huge(in, in_nbytes);

    InputStream in_stream(in, in_nbytes);

    fprintf(stderr, "Thread %lu started\n", skip);
    if (skip == 0)
        decompress_first_chunk(d, in_stream, stop);
    else {
        assert(prev_sync);
        decompress_chunks(d, in_stream, skip, stop, *prev_sync);
    }
    return LIBDEFLATE_SUCCESS;
}
