kb_dir = meson.current_source_dir()

euart0_args = ['-DRF_EUART0=1']
if get_option('rf_debug_keep_usb')
    if get_option('buildtype') != 'debug'
        error('aula-f75: rf_debug_keep_usb needs a debug build - it exists only to keep the HID console alive')
    endif
    euart0_args += '-DRF_DEBUG_KEEP_USB=1'
endif

wireless_modules += {
    'euart0' : {
        'src' : files('aula_rf.c'),
        'inc' : [],
        'args' : euart0_args,
    },
}

keyboards += {
    'aula-f75' : {
        'dir' : kb_dir,
        'platform' : 'sh68f90',
        'icp_part' : 'sh68f90a',  # the part name sinodude programs it as, over ICP
        'src' : files(
            'user_matrix.c',
            'user_init.c',
            'user_sleep.c',
            'kb.c',
            'aula_rgb.c',
            'aula_fx.c',
            'aula_encoder.c',
            'aula_macro.c',
            'aula_status.c',
        ),
        'boot_src' : files('user_matrix.c', 'user_init.c'),
        'layouts' : {
            'default' : {
                'dir' : kb_dir / 'layouts/default',
                'src' : files(
                    'layouts/default/indicators.c',
                    'layouts/default/layout.c',
                ),
            },
        },
        'options' : {
            'vendor_id' : '0x258a',
            'product_id' : '0x010c',
            'wireless' : 'euart0',
            'led_rows' : '6',
            'led_cols' : '15',
            'led_cx2' : '14',
            'led_cy2' : '5',
        },
    },
}
