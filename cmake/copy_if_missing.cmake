# Copy SRC to DST only when DST does not exist yet — used for staging
# user-editable templates (e.g. assets/music/manifest.txt) next to the
# executable without clobbering the user's edits on rebuild.
# Usage: cmake -DSRC=<file> -DDST=<file> -P copy_if_missing.cmake
if(NOT EXISTS "${DST}")
    file(COPY_FILE "${SRC}" "${DST}")
endif()
