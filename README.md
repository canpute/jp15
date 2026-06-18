# jp15

Python bindings for HTJ2K (JPEG Part 15) via OpenJPH.

The `jp15` Python library provides support for encoding and decoding
High-throughput JPEG2000 (HTJ2K), also known as JPH, JPEG2000 Part 15,
ISO/IEC 15444-15, and ITU-T T.814. So many acronyms!


## What is High-throughput JPEG2000 (HTJ2K)?

JPEG2000 is a file format with some nice properties compared to "common jpeg",
including a much better quality at the same bitrate. The downside is that encoding
is much slower. HTJ2K fixes that, resulting in compression that is on
par of faster than turbojpeg (so I've heard).


## Technical details

This wrapper uses cffi to hook into the popular [openjph](https://github.com/aous72/OpenJPH)
library. Since cffi requires a C API, while openjpg is C++, this project
also implements a C-API for using openjph. The advantage of using cffi
is that the resuling binary wheels work on a wide range of Python versions,
including future versions and alternative Python implementations like Pypy.


## Installation

Binary wheels are provided via Pypi, just use your favourite tool to install:
```
pip install jp15
```


## Developers

Installation:
```
git clone ..
cd jp15
pip install -e .
```

Compile the lib:
```
python build.py
```


## License

MIT
