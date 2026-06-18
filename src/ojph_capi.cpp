/**
 * ojph_capi.cpp
 * Implementation of the C shim declared in ojph_capi.h.
 *
 * Verified against OpenJPH v0.29.0 headers:
 *   ojph_codestream.h, ojph_params.h, ojph_file.h, ojph_message.h
 */

#include "ojph_capi.h"

#include "ojph_file.h"
#include "ojph_mem.h"
#include "ojph_codestream.h"
#include "ojph_params.h"
#include "ojph_message.h"
#include "ojph_version.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <stdexcept>
#include <string>

/* =========================================================================
 * Internal helpers
 * ====================================================================== */

namespace {

static const size_t ERR_BUF = 512;

// Per-call error capture: we derive from message_error and override
// operator() to capture the message instead of printing it.
// The operator() MUST throw — OpenJPH relies on the exception to unwind.
// We throw a std::runtime_error which our catch(...) blocks handle.
struct CapturingErrorHandler : public ojph::message_error {
    char msg[ERR_BUF];

    CapturingErrorHandler() { msg[0] = '\0'; }

    void operator()(int /*code*/, const char* /*file*/, int /*line*/,
                    const char* fmt, ...) override
    {
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(msg, ERR_BUF, fmt, args);
        va_end(args);
        throw std::runtime_error(msg);
    }
};

// Suppress warnings and info during normal operation — redirect to /dev/null.
// We install these once globally; they don't need to be per-call.
struct NullWarningHandler : public ojph::message_warning {
    void operator()(int, const char*, int, const char*, ...) override {}
};

struct NullInfoHandler : public ojph::message_info {
    void operator()(int, const char*, int, const char*, ...) override {}
};

static NullWarningHandler g_null_warning;
static NullInfoHandler    g_null_info;

static void install_null_handlers() {
    static bool done = false;
    if (!done) {
        ojph::configure_warning(&g_null_warning);
        ojph::configure_info(&g_null_info);
        done = true;
    }
}

inline uint32_t bytes_per_sample(uint32_t bit_depth) {
    return (bit_depth <= 8) ? 1u : 2u;
}

} // anonymous namespace

/* =========================================================================
 * Handle structs
 * ====================================================================== */

struct ojph_decoder_s {
    char              err[ERR_BUF];
    ojph::codestream  cs;
    ojph::mem_infile  infile;
    ojph_image_info_t info;
    bool              headers_read = false;

    ojph_decoder_s() { err[0] = '\0'; }
};

struct ojph_encoder_s {
    char                 err[ERR_BUF];
    ojph_encode_params_t params;
    bool                 params_set = false;

    ojph_encoder_s() { err[0] = '\0'; }
};

/* =========================================================================
 * Version
 * ====================================================================== */

extern "C"
const char* ojph_version(void) {
    // OPENJPH_VERSION_MAJOR/MINOR/PATCH exist; compose a static string.
    static char v[32];
    if (v[0] == '\0')
        std::snprintf(v, sizeof(v), "%d.%d.%d",
                      OPENJPH_VERSION_MAJOR,
                      OPENJPH_VERSION_MINOR,
                      OPENJPH_VERSION_PATCH);
    return v;
}

/* =========================================================================
 * Decoder
 * ====================================================================== */

extern "C"
ojph_decoder_t* ojph_decoder_create(void) {
    install_null_handlers();
    return new (std::nothrow) ojph_decoder_t();
}

extern "C"
void ojph_decoder_destroy(ojph_decoder_t* dec) {
    delete dec;
}

extern "C"
int ojph_decoder_read_headers(ojph_decoder_t* dec,
                              const uint8_t*  data,
                              size_t          length)
{
    if (!dec || !data || length == 0) return OJPH_ERROR_PARAM;

    CapturingErrorHandler err_handler;
    ojph::configure_error(&err_handler);

    try {
        dec->headers_read = false;
        // mem_infile::open takes const ui8* — no cast needed
        dec->infile.open(data, length);
        dec->cs.read_headers(&dec->infile);

        ojph::param_siz siz = dec->cs.access_siz();

        // Image dimensions: extent - offset
        ojph::point extent = siz.get_image_extent();
        ojph::point offset = siz.get_image_offset();
        dec->info.width          = extent.x - offset.x;
        dec->info.height         = extent.y - offset.y;
        dec->info.num_components = siz.get_num_components();

        // Component 0 for bit depth / signedness
        dec->info.bit_depth  = siz.get_bit_depth(0);
        dec->info.is_signed  = siz.is_signed(0) ? 1 : 0;

        ojph::param_cod cod = dec->cs.access_cod();
        dec->info.is_reversible = cod.is_reversible() ? 1 : 0;

        dec->headers_read = true;
        return OJPH_OK;
    }
    catch (const std::exception& e) {
        std::snprintf(dec->err, ERR_BUF, "%s", e.what());
        return OJPH_ERROR_DECODE;
    }
    catch (...) {
        std::snprintf(dec->err, ERR_BUF, "unknown exception in read_headers");
        return OJPH_ERROR_DECODE;
    }
}

extern "C"
int ojph_decoder_get_info(ojph_decoder_t*    dec,
                          ojph_image_info_t* info)
{
    if (!dec || !info)       return OJPH_ERROR_PARAM;
    if (!dec->headers_read)  return OJPH_ERROR_DECODE;
    *info = dec->info;
    return OJPH_OK;
}

extern "C"
int ojph_decoder_decode(ojph_decoder_t* dec,
                        uint8_t**       planes,
                        uint32_t        num_planes,
                        uint32_t        stride)
{
    if (!dec || !planes)                              return OJPH_ERROR_PARAM;
    if (!dec->headers_read)                           return OJPH_ERROR_DECODE;
    if (num_planes != dec->info.num_components)       return OJPH_ERROR_PARAM;

    CapturingErrorHandler err_handler;
    ojph::configure_error(&err_handler);

    const uint32_t W   = dec->info.width;
    const uint32_t H   = dec->info.height;
    const uint32_t nc  = dec->info.num_components;
    const uint32_t bps = bytes_per_sample(dec->info.bit_depth);
    const uint32_t row_bytes = (stride == 0) ? (W * bps) : stride;

    try {
        dec->cs.create();

        // pull() returns the next available line and tells us which
        // component it belongs to via the output parameter.
        // OpenJPH interleaves components by default (non-planar).
        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t c = 0; c < nc; ++c) {
                ojph::ui32 comp_idx = 0;
                ojph::line_buf* line = dec->cs.pull(comp_idx);
                if (!line) {
                    std::snprintf(dec->err, ERR_BUF, "pull() returned null at y=%u c=%u", y, c);
                    return OJPH_ERROR_DECODE;
                }

                uint8_t* dst = planes[comp_idx] + (size_t)y * row_bytes;
                const ojph::si32* src = line->i32;

                if (bps == 1) {
                    for (uint32_t x = 0; x < W; ++x) {
                        ojph::si32 v = src[x];
                        if (dec->info.is_signed)
                            v += (1 << (dec->info.bit_depth - 1));
                        dst[x] = (uint8_t)std::clamp(v, (ojph::si32)0, (ojph::si32)255);
                    }
                } else {
                    uint16_t* dst16 = reinterpret_cast<uint16_t*>(dst);
                    ojph::si32 max_val = (1 << dec->info.bit_depth) - 1;
                    for (uint32_t x = 0; x < W; ++x) {
                        ojph::si32 v = src[x];
                        if (dec->info.is_signed)
                            v += (1 << (dec->info.bit_depth - 1));
                        dst16[x] = (uint16_t)std::clamp(v, (ojph::si32)0, max_val);
                    }
                }
            }
        }

        dec->cs.close();
        return OJPH_OK;
    }
    catch (const std::exception& e) {
        std::snprintf(dec->err, ERR_BUF, "%s", e.what());
        return OJPH_ERROR_DECODE;
    }
    catch (...) {
        std::snprintf(dec->err, ERR_BUF, "unknown exception during decode");
        return OJPH_ERROR_DECODE;
    }
}

extern "C"
const char* ojph_decoder_last_error(const ojph_decoder_t* dec) {
    if (!dec) return "null decoder handle";
    return dec->err;
}

/* =========================================================================
 * Encoder
 * ====================================================================== */

extern "C"
ojph_encoder_t* ojph_encoder_create(void) {
    install_null_handlers();
    return new (std::nothrow) ojph_encoder_t();
}

extern "C"
void ojph_encoder_destroy(ojph_encoder_t* enc) {
    delete enc;
}

extern "C"
int ojph_encoder_set_params(ojph_encoder_t*             enc,
                            const ojph_encode_params_t* params)
{
    if (!enc || !params) return OJPH_ERROR_PARAM;
    enc->params     = *params;
    enc->params_set = true;
    return OJPH_OK;
}

extern "C"
int ojph_encoder_encode(ojph_encoder_t* enc,
                        const uint8_t** planes,
                        uint32_t        num_planes,
                        uint32_t        stride,
                        uint8_t*        out_data,
                        size_t          out_capacity,
                        size_t*         out_length)
{
    if (!enc || !planes || !out_data || !out_length) return OJPH_ERROR_PARAM;
    if (!enc->params_set)                            return OJPH_ERROR_PARAM;
    if (num_planes != enc->params.num_components)    return OJPH_ERROR_PARAM;

    CapturingErrorHandler err_handler;
    ojph::configure_error(&err_handler);

    const ojph_encode_params_t& p = enc->params;
    const uint32_t W   = p.width;
    const uint32_t H   = p.height;
    const uint32_t nc  = p.num_components;
    const uint32_t bps = bytes_per_sample(p.bit_depth);
    const uint32_t row_bytes = (stride == 0) ? (W * bps) : stride;

    *out_length = 0;

    try {
        ojph::codestream cs;

        // --- SIZ ---
        ojph::param_siz siz = cs.access_siz();
        siz.set_image_offset(ojph::point(0, 0));
        siz.set_tile_size(ojph::size(W, H));
        siz.set_tile_offset(ojph::point(0, 0));
        siz.set_image_extent(ojph::point(W, H));
        siz.set_num_components(nc);
        for (uint32_t c = 0; c < nc; ++c)
            siz.set_component(c, ojph::point(1, 1), p.bit_depth, p.is_signed != 0);

        // --- COD ---
        ojph::param_cod cod = cs.access_cod();
        cod.set_num_decomposition(p.num_decomps > 0 ? p.num_decomps : 5);
        cod.set_reversible(p.is_reversible != 0);

        // Progression order as string: 0=LRCP 1=RLCP 2=RPCL 3=PCRL 4=CPRL
        static const char* prog_names[] = {"LRCP","RLCP","RPCL","PCRL","CPRL"};
        uint32_t prog_idx = std::min<uint32_t>(p.progression, 4);
        cod.set_progression_order(prog_names[prog_idx]);

        // Color transform for 3-component images
        if (nc == 3)
            cod.set_color_transform(true);

        // --- QCD (irreversible only) ---
        if (!p.is_reversible && p.qstep > 0.f) {
            ojph::param_qcd qcd = cs.access_qcd();
            qcd.set_irrev_quant(p.qstep);
        }

        // --- Encode ---
        ojph::mem_outfile outfile;
        outfile.open();
        cs.write_headers(&outfile);

        for (uint32_t y = 0; y < H; ++y) {
            for (uint32_t c = 0; c < nc; ++c) {
                // First call with nullptr gets an empty line_buf and tells
                // us which component to fill via next_component.
                ojph::ui32 next_comp = 0;
                ojph::line_buf* line = cs.exchange(nullptr, next_comp);
                if (!line) {
                    std::snprintf(enc->err, ERR_BUF,
                                  "exchange() returned null at y=%u c=%u", y, c);
                    return OJPH_ERROR_ENCODE;
                }

                const uint8_t* src = planes[next_comp] + (size_t)y * row_bytes;
                ojph::si32* dst = line->i32;

                if (bps == 1) {
                    for (uint32_t x = 0; x < W; ++x) {
                        dst[x] = (ojph::si32)(uint8_t)src[x];
                        if (p.is_signed)
                            dst[x] -= (1 << (p.bit_depth - 1));
                    }
                } else {
                    const uint16_t* src16 =
                        reinterpret_cast<const uint16_t*>(src);
                    for (uint32_t x = 0; x < W; ++x) {
                        dst[x] = (ojph::si32)src16[x];
                        if (p.is_signed)
                            dst[x] -= (1 << (p.bit_depth - 1));
                    }
                }

                // Second call submits the filled line.
                cs.exchange(line, next_comp);
            }
        }

        cs.flush();
        cs.close();

        size_t written = (size_t)outfile.tell();
        if (written > out_capacity) {
            std::snprintf(enc->err, ERR_BUF, "output buffer too small");
            return OJPH_ERROR_ENCODE;
        }
        std::memcpy(out_data, outfile.get_data(), written);
        *out_length = written;

        return OJPH_OK;
    }
    catch (const std::exception& e) {
        std::snprintf(enc->err, ERR_BUF, "%s", e.what());
        return OJPH_ERROR_ENCODE;
    }
    catch (...) {
        std::snprintf(enc->err, ERR_BUF, "unknown exception during encode");
        return OJPH_ERROR_ENCODE;
    }
}

extern "C"
const char* ojph_encoder_last_error(const ojph_encoder_t* enc) {
    if (!enc) return "null encoder handle";
    return enc->err;
}