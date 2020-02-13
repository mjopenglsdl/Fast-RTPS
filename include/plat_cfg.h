#if defined(__MINGW64__)
    #undef _WIN32
#else
    #error MACRO FAILED
#endif