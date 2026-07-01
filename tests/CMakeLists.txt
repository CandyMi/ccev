if(BUILD_TESTING)
    enable_testing()

    function(add_ccev_test name source)
        add_executable(${name} ${source})
        target_link_libraries(${name} PRIVATE ccev)
        # Required for Xcode iOS/tvOS/visionOS builds — every executable
        # bundle needs PRODUCT_BUNDLE_IDENTIFIER.  Harmless on other
        # generators (MSBuild, Ninja, Makefiles — ignored).
        set_target_properties(${name} PROPERTIES
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.ccev.${name}"
            XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO")
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    add_ccev_test(test_oom    test_oom.c)
    add_ccev_test(test_timer  test_timer.c)
    add_ccev_test(test_sock   test_sock.c)
    add_ccev_test(test_stream test_stream.c)
    add_ccev_test(test_dns    test_dns.c)
    add_ccev_test(test_icmp   test_icmp.c)
    add_ccev_test(test_signal test_signal.c)
endif()
