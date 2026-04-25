# GLAD (deferred)

This directory is reserved for a generated GLAD GL 3.3 core profile loader.
Phase 0b's empty binary uses the SDL2 renderer (no direct GL), so GLAD is not
yet compiled in. Phase 1b is the first time we need raw GL — generate then.

To regenerate (when needed):

1. Visit https://glad.dav1d.de — Language: C/C++, gl: Version 3.3, Profile: Core,
   Extensions: pick what PsyCross needs (FBO, etc.), Generate a loader, omit KHR.
2. Drop the generated `include/` and `src/` here, replacing this README.
3. Add an `ORIGIN.txt` recording the generator timestamp + parameters.
