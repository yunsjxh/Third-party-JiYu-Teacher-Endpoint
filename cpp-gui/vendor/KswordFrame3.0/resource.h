#ifndef KSWORDFRAME_RESOURCE_H
#define KSWORDFRAME_RESOURCE_H

// Centralized Windows resource identifiers for application branding.
// Inputs are replaceable project-local file names; the .rc file embeds them,
// and KTitleBar.cpp reuses the same macros when it needs a disk fallback.
#define IDI_KSWORD_APP_ICON        101
#define IDR_KSWORD_APP_ICON_ICO    201
#define IDR_KSWORD_APP_LOGO_PNG    202

#define KSWORD_APP_ICON_FILE       "Resources\\app.ico"
#define KSWORD_APP_LOGO_FILE       "Resources\\app_logo.png"

#endif // KSWORDFRAME_RESOURCE_H
