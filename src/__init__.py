"""
jp15 — Python bindings for HTJ2K (JPEG Part 15) via OpenJPH.
"""

from .core import (
    decode,
    encode,Jp15Error
)

__all__ = ["decode", "encode", "Jp15Error"]
