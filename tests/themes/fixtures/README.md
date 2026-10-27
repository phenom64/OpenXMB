# Scripted theme fixture

`native-scripted.p3t` is generated entirely from test data: a gradient GTF texture, a compressed Edge textured quad, a VSMX interval timer with sinusoidal movement, and a generated PNG still/preview. It contains no extracted console artwork or downloaded theme resources.

To regenerate, create the raw resources and then wrap them in the binary resource trees:

```sh
python3 tests/themes/fixtures/generate_raf.py /tmp/openxmb-raf-fixtures
python3 tests/themes/fixtures/wrap_fixture.py /tmp/openxmb-raf-fixtures tests/themes/fixtures/native-scripted.p3t
```

The wrapper builds actual P3TF and RAFO binary resource trees and compresses their payloads. The library test imports the resulting file and verifies that native projected vertices move over time, rather than substituting an already-decoded JSON scene.
