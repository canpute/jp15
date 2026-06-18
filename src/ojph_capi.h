/**
 * ojph_capi.h
 * C API shim over OpenJPH's C++ interface, suitable for cffi ABI-mode binding.
 *
 * Design principles
 * -----------------
 * - All types are opaque handles (pointers to forward-declared structs).
 * - No C++ types, templates, exceptions, or namespaces cross this boundary.
 * - Errors are communicated via return codes (OJPH_OK / negative values) and
 *   an optional per-handle error string retrievable with ojph_last_error().
 * - Memory for decoded pixels is caller-supplied (zero-copy into numpy arrays).
 * - All functions are declared extern "C" in the .cpp implementation.
 *

 */

#ifndef OJPH_CAPI_H
#define OJPH_CAPI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Version
 * ---------------------------------------------------------------------- */

/** Returns the OpenJPH library version string, e.g. 3029001 for v3.29.1. */
const char* ojph_version(void);

/* -------------------------------------------------------------------------
 * Error codes
 * ---------------------------------------------------------------------- */

#define OJPH_OK               0
#define OJPH_ERROR_GENERAL   -1
#define OJPH_ERROR_IO        -2
#define OJPH_ERROR_DECODE    -3
#define OJPH_ERROR_ENCODE    -4
#define OJPH_ERROR_PARAM     -5

/* -------------------------------------------------------------------------
 * Opaque handle types
 * ---------------------------------------------------------------------- */

typedef struct ojph_decoder_s ojph_decoder_t;
typedef struct ojph_encoder_s ojph_encoder_t;

/* -------------------------------------------------------------------------
 * Image geometry descriptor (filled in by ojph_decoder_get_info)
 * ---------------------------------------------------------------------- */

typedef struct {
    uint32_t width;           /* image width in pixels (at full resolution)  */
    uint32_t height;          /* image height in pixels                       */
    uint32_t num_components;  /* number of colour components, e.g. 1 or 3    */
    uint32_t bit_depth;       /* bits per sample, e.g. 8, 12, 16             */
    int      is_signed;       /* non-zero if samples are signed integers      */
    int      is_reversible;   /* non-zero if lossless (5/3 wavelet)           */
} ojph_image_info_t;

/* -------------------------------------------------------------------------
 * Encoder parameters (passed to ojph_encoder_set_params)
 * ---------------------------------------------------------------------- */

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t num_components;
    uint32_t bit_depth;
    int      is_signed;
    int      is_reversible;    /* 1 = lossless 5/3, 0 = lossy 9/7            */
    float    qstep;            /* quantisation step (ignored if reversible)   */
    uint32_t num_decomps;      /* wavelet decomposition levels, default 5     */
    uint32_t progression;      /* 0=LRCP 1=RLCP 2=RPCL 3=PCRL 4=CPRL        */
} ojph_encode_params_t;

/* -------------------------------------------------------------------------
 * Decoder lifecycle
 * ---------------------------------------------------------------------- */

/**
 * Create a new decoder handle.
 * Returns NULL on allocation failure.
 */
ojph_decoder_t* ojph_decoder_create(void);

/**
 * Destroy a decoder handle created with ojph_decoder_create().
 * Safe to call with NULL.
 */
void ojph_decoder_destroy(ojph_decoder_t* dec);

/**
 * Read headers from an in-memory HTJ2K / J2K / JPH codestream.
 *
 * @param dec     Decoder handle.
 * @param data    Pointer to compressed data.
 * @param length  Byte length of compressed data.
 * @returns OJPH_OK on success, negative error code on failure.
 *
 * Call ojph_decoder_get_info() after this to learn image dimensions.
 */
int ojph_decoder_read_headers(ojph_decoder_t* dec,
                              const uint8_t*  data,
                              size_t          length);

/**
 * Query image geometry after a successful ojph_decoder_read_headers().
 *
 * @param dec   Decoder handle.
 * @param info  Output struct; filled on OJPH_OK return.
 */
int ojph_decoder_get_info(ojph_decoder_t*    dec,
                          ojph_image_info_t* info);

/**
 * Decode the codestream into caller-supplied buffers.
 *
 * The caller is responsible for allocating one buffer per component.
 * Each buffer must be at least (width * height * bytes_per_sample) bytes,
 * where bytes_per_sample = (bit_depth <= 8) ? 1 : 2.
 *
 * Samples are stored row-major, left-to-right, top-to-bottom.
 * For 9–16 bit depths the 16-bit values are packed as native-endian uint16.
 *
 * @param dec          Decoder handle (headers already read).
 * @param planes       Array of num_components pointers to output buffers.
 * @param num_planes   Must match num_components from ojph_image_info_t.
 * @param stride       Row stride in bytes for each plane (pass 0 for tightly
 *                     packed, i.e. width * bytes_per_sample).
 * @returns OJPH_OK on success, negative error code on failure.
 */
int ojph_decoder_decode(ojph_decoder_t* dec,
                        uint8_t**       planes,
                        uint32_t        num_planes,
                        uint32_t        stride);

/**
 * Return a human-readable description of the last error on this handle.
 * The pointer is valid until the next API call on the same handle.
 */
const char* ojph_decoder_last_error(const ojph_decoder_t* dec);

/* -------------------------------------------------------------------------
 * Encoder lifecycle
 * ---------------------------------------------------------------------- */

/**
 * Create a new encoder handle.
 * Returns NULL on allocation failure.
 */
ojph_encoder_t* ojph_encoder_create(void);

/**
 * Destroy an encoder handle.
 * Safe to call with NULL.
 */
void ojph_encoder_destroy(ojph_encoder_t* enc);

/**
 * Configure encoder parameters before the first call to ojph_encoder_encode().
 *
 * @param enc     Encoder handle.
 * @param params  Encoding parameters; all fields must be initialised.
 * @returns OJPH_OK or negative error code.
 */
int ojph_encoder_set_params(ojph_encoder_t*           enc,
                            const ojph_encode_params_t* params);

/**
 * Encode planar image data into an HTJ2K codestream.
 *
 * Input planes are in the same layout as ojph_decoder_decode() output:
 * one uint8_t* per component, row-major, with optional stride.
 *
 * The output buffer must be pre-allocated by the caller.  A safe upper
 * bound is (width * height * num_components * bytes_per_sample * 2).
 * The actual number of bytes written is returned in *out_length.
 *
 * TODO: is that actually a good idea? There will always be over-allocation??
 *
 * @param enc          Encoder handle (params already set).
 * @param planes       Array of input plane pointers.
 * @param num_planes   Number of planes (must match params.num_components).
 * @param stride       Row stride in bytes for each plane (0 = tightly packed).
 * @param out_data     Caller-allocated output buffer.
 * @param out_capacity Size of out_data in bytes.
 * @param out_length   Set to the number of bytes written on success.
 * @returns OJPH_OK on success, negative error code on failure.
 */
int ojph_encoder_encode(ojph_encoder_t* enc,
                        const uint8_t** planes,
                        uint32_t        num_planes,
                        uint32_t        stride,
                        uint8_t*        out_data,
                        size_t          out_capacity,
                        size_t*         out_length);

/**
 * Return a human-readable description of the last error on this handle.
 */
const char* ojph_encoder_last_error(const ojph_encoder_t* enc);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* OJPH_CAPI_H */