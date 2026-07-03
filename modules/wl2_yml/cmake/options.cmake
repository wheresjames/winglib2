if(COMMAND wl2_module_option)
    wl2_module_option(ENABLE DOC "Build wl2_yml YAML module (vendors yaml-cpp)")
endif()

# yaml-cpp is vendored from source. "off" disables the module; any other value
# fetches and compiles the pinned release into the module.
set(WL2_YAMLCPP_PROVIDER "fetch" CACHE STRING "yaml-cpp provider: fetch or off")
set_property(CACHE WL2_YAMLCPP_PROVIDER PROPERTY STRINGS fetch off)
set(WL2_YAMLCPP_VERSION "0.8.0" CACHE STRING "yaml-cpp release version")
set(WL2_YAMLCPP_URL "https://github.com/jbeder/yaml-cpp/archive/refs/tags/${WL2_YAMLCPP_VERSION}.tar.gz" CACHE STRING "yaml-cpp source archive URL")
set(WL2_YAMLCPP_URL_HASH "SHA256=fbe74bbdcee21d656715688706da3c8becfd946d92cd44705cc6098bb23b3a16" CACHE STRING "yaml-cpp source archive hash")
