from .lib import ffi, capi

import numpy as np


# -- Helpers


class Checker:

    def __init__(self, encoder=None, decoder=None):
        self._ob = encoder or decoder
        self._func =  capi.ojph_encoder_last_error if encoder else capi.ojph_decoder_last_error

    def check(self, return_code: int):
        if return_code != 0:  # OJPH_OK
            c_str = self._func(self._ob)
            msg = ffi.string(c_str).decode("utf-8", errors="replace")
            raise Jp15Error(f"libojph_capi error (code {return_code}): {msg}")


class Jp15Error(RuntimeError):
    """Raised when the underlying OpenJPH C library returns an error."""


def _dtype_for_bit_depth(bit_depth: int, is_signed: int) -> np.dtype:
    if bit_depth <= 8:
        return np.dtype(np.int8 if is_signed else np.uint8)
    else:
        return np.dtype(np.int16 if is_signed else np.uint16)


def _bit_depth_for_dtype(dtype: np.dtype) -> tuple[int, bool]:
    """Return (bit_depth, is_signed) for a numpy dtype."""
    info = np.iinfo(dtype)
    return info.bits, np.issubdtype(dtype, np.signedinteger)


# -- Public API

# def openjph_version() -> str:
#     """Return the OpenJPH library version string, e.g. '0.29.0'."""
#     return ffi.string(capi.ojph_version()).decode()


def decode(data: Union[bytes, bytearray, memoryview, np.ndarray],
           *,
           planar: bool = False) -> np.ndarray:
    """Decode an HTJ2K / JPH codestream and return an image.

    Parameters
    ----------
    data:
        Compressed codestream as bytes, bytearray, memoryview, or a 1-D
        uint8 numpy array.
    planar:
        If False (default) return shape (H, W) for single-component or
        (H, W, C) for multi-component images — interleaved / HWC layout.
        If True return shape (H, W) or (C, H, W) — planar / CHW layout.

    Returns
    -------
    numpy.ndarray
        uint8 for 8-bit images, uint16 for 9–16 bit images.
    """

    # Normalise input to a contiguous uint8 buffer
    if isinstance(data, np.ndarray):
        data = np.ascontiguousarray(data, dtype=np.uint8)
        buf = ffi.from_buffer(data)
        # TODO: memoryview no copy
    else:
        buf = ffi.from_buffer(bytes(data))
    length = len(buf)

    # Create a decoder object
    dec = capi.ojph_decoder_create()
    if dec == ffi.NULL:
        raise MemoryError("ojph_decoder_create() returned NULL")

    try:
        checker = Checker(decoder=dec)

        # Read headers
        rc = capi.ojph_decoder_read_headers(dec, buf, length)
        checker.check(rc)

        # Get info on the image
        info = ffi.new("ojph_image_info_t *")
        rc = capi.ojph_decoder_get_info(dec, info)
        checker.check(rc)
        w, h  = info.width, info.height
        nc = info.num_components
        dtype = _dtype_for_bit_depth(info.bit_depth, info.is_signed)

        # Allocate array (C-contiguous, tight stride)
        plane_arrays = np.empty((nc, h, w), dtype=dtype)
        plane_ptrs  = ffi.new("uint8_t*[]", nc)
        for i in range(nc):
            view = plane_arrays[i]
            plane_ptrs[i] = ffi.cast("uint8_t*", ffi.from_buffer(view))

        # Decode into our arrays
        rc = capi.ojph_decoder_decode(dec, plane_ptrs, nc, 0)
        checker.check(rc)

    finally:
        capi.ojph_decoder_destroy(dec)

    # Assemble output array
    if nc == 1:
        return plane_arrays[0]  # (H, W)
    elif planar:
        return plane_arrays
    else:
        # TODO: this copies data, can we avoid this?
        return np.stack(plane_arrays, axis=-1)  # (H, W, C)


def encode(image: np.ndarray,
           *,
           lossless: bool = True,
           qstep: float = 0.01,
           num_decomps: int = 5,
           planar: bool = False) -> bytes:
    """Encode a numpy array as an HTJ2K codestream.

    Parameters
    ----------
    image:
        uint8 or uint16 numpy array.
        Shape (H, W) for grayscale, (H, W, C) for interleaved multi-component
        (default), or (C, H, W) when *planar* is True.
    lossless:
        If True (default) use the reversible 5/3 wavelet transform.
        If False use the irreversible 9/7 transform with quantisation step
        *qstep*.
    qstep:
        Quantisation step size for lossy encoding (ignored when lossless=True).
        Smaller values → better quality, larger file.
    num_decomps:
        Number of wavelet decomposition levels (default 5).
    planar:
        Set to True if *image* is in (C, H, W) layout rather than (H, W, C).

    Returns
    -------
    bytes
        Raw HTJ2K codestream.
    """

    # Check input
    # TODO: allow memoryview as well,
    # TODO: wait let's not make numpy a dependency?
    if not isinstance(image, np.ndarray):
        raise TypeError(f"image must be a numpy array, got {type(image)}")
    if image.dtype not in (np.uint8, np.int8, np.uint16, np.int16):
        raise TypeError(f"image dtype must be uint8, int8, uint16, or int16; got {image.dtype}")

    # Normalise to list of (H, W) plane arrays
    if image.ndim == 2:
        planes = [np.ascontiguousarray(image)]
    elif image.ndim == 3:
        if planar:
            # (C, H, W) → list of (H, W)
            planes = [np.ascontiguousarray(image[c]) for c in range(image.shape[0])]
        else:
            # TODO: can avoid copy?
            # (H, W, C) → list of (H, W)
            planes = [np.ascontiguousarray(image[:, :, c]) for c in range(image.shape[2])]
    else:
        raise ValueError(f"image must be 2-D or 3-D, got shape {image.shape}")

    h, w = planes[0].shape
    nc   = len(planes)
    bit_depth, is_signed = _bit_depth_for_dtype(image.dtype)

    # Create an encoder object
    enc = capi.ojph_encoder_create()
    if enc == ffi.NULL:
        raise MemoryError("ojph_encoder_create() returned NULL")

    try:
        checker = Checker(encoder=enc)

        # Prepare info
        params = ffi.new("ojph_encode_params_t *")
        params.width          = w
        params.height         = h
        params.num_components = nc
        params.bit_depth      = bit_depth
        params.is_signed      = int(is_signed)
        params.is_reversible  = int(lossless)
        params.qstep          = float(qstep)
        params.num_decomps    = num_decomps
        params.progression    = 0  # LRCP

        # Push the info to the encoder
        rc = capi.ojph_encoder_set_params(enc, params)
        checker.check(rc)

        # Allocate a worst-case output buffer
        # TODO: can we not simply let openjph allocate it for us?
        bps = 1 if bit_depth <= 8 else 2
        out_capacity = w * h * nc * bps * 2
        out_buf  = ffi.new("uint8_t[]", out_capacity)
        out_len  = ffi.new("size_t *")
        plane_ptrs = ffi.new("const uint8_t*[]", nc)
        for i, arr in enumerate(planes):
            plane_ptrs[i] = ffi.cast("const uint8_t*", ffi.from_buffer(arr))

        rc =    capi.ojph_encoder_encode(enc, plane_ptrs, nc, 0,
                                     out_buf, out_capacity, out_len)
        checker.check(rc)
    finally:
        capi.ojph_encoder_destroy(enc)

    # TODO: avoid copy here by casting to bytes, better return a memoryview or something
    return bytes(ffi.buffer(out_buf, out_len[0]))