export module openxmb.debug;

export namespace openxmb::debug {
    // Global toggle for interface/UI graphics debugging
    inline bool interfacefx_debug = false;
    // Helper to avoid repeating logs every frame
    inline bool interfacefx_debug_once_atlas_logged = false;
}
