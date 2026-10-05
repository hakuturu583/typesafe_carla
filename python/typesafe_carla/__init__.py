"""Python-side tooling for typesafe_carla.

The CARLA API itself lives in the Codon modules (``import typesafe_carla`` inside
a Codon program). This Python package only locates the Codon compiler, the
Codon sources and the native library, and provides the ``typesafe-codon``
launcher. It never imports the CARLA Python package.
"""

__version__ = "0.2.0"
