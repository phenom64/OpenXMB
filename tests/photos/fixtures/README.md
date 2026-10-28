# Photo decoder fixtures

These synthetic 4 x 2 red/blue JPEGs contain no user photographs. They were
created with Pillow 12.2.0 at quality 95 with chroma subsampling disabled.

- baseline.jpg: baseline RGB JPEG with EXIF orientation 6 (90 degrees clockwise).
- progressive.jpeg: progressive RGB JPEG without EXIF orientation.

Tests copy these into temporary directories, including Unicode paths and names
with misleading or missing extensions. Pillow is not needed to run the tests.
