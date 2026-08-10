if(COMMAND wl2_module_option)
    wl2_module_option(ENABLE DEFAULT OFF DOC "Build the wl2:onvif client module")
endif()

set(WL2_ONVIF_PROVIDER "auto" CACHE STRING "wlonvif provider: auto, local, package, fetch, or off")
set_property(CACHE WL2_ONVIF_PROVIDER PROPERTY STRINGS auto local package fetch off)
set(WL2_ONVIF_COMMIT "a91bec0aa2aca6209a9a6c493933dc98db4b191e" CACHE STRING "Pinned wlonvif commit")
set(WL2_ONVIF_URL
    "https://github.com/wheresjames/wlonvif/archive/${WL2_ONVIF_COMMIT}.tar.gz"
    CACHE STRING "Pinned wlonvif source archive")
set(WL2_ONVIF_URL_HASH
    "SHA256=8781a36c7554ffd49d1bb217a5946eed6a5fad4ec10a6a867e416d8dcae688cf"
    CACHE STRING "Pinned wlonvif source archive hash")
if(WL2_DEPS_ROOT)
    set(WL2_ONVIF_ROOT "${WL2_DEPS_ROOT}/wlonvif" CACHE PATH "Local wlonvif package or source tree")
endif()
