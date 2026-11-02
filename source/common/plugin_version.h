#pragma once

// Edit only the requested plugin's four-part version; do not bump other plugins.
#define TCAL_VERSION (1,0,0,0)
#define TCAP_VERSION (1,0,0,0)
#define TCARD_VERSION (1,0,0,0)
#define TCYC_VERSION (1,0,0,0)
#define SAMPLE_FORMATTER_VERSION (1,0,0,0)
#define SAMPLE_CHIME_VERSION (1,0,0,0)
#define SAMPLE_RENDER_VERSION (1,0,0,0)

// Tuple expansion works with the Windows resource compiler's traditional preprocessor.
#define TC_VERSION_PARTS(major, minor, patch, revision) major,minor,patch,revision
#define TC_VERSION_STRINGS(major, minor, patch, revision) #major "." #minor "." #patch "." #revision
#define TC_VERSION_VALUE(version) TC_VERSION_PARTS version
#define TC_VERSION_TEXT(version) TC_VERSION_STRINGS version
