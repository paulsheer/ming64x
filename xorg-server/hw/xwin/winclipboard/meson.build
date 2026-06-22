srcs_windows_clipboard = [
    'winclipboard.h',
    'textconv.c',
    'imageconv.c',
    'thread.c',
    'wndproc.c',
    'xevents.c',
]

xwin_clipboard = static_library(
    'XWinclipboard',
    srcs_windows_clipboard,
    include_directories: inc,
    c_args: '-DHAVE_XWIN_CONFIG_H',
    dependencies: [
        dependency('xcb'),
        dependency('xcb-aux'),
        dependency('xcb-icccm'),
        dependency('xcb-xfixes'),
        socket_dep,
    ],
)

srcs_xwinclip = [
    'xwinclip.c',
    'debug.c',
]

executable(
    'xwinclip',
    srcs_xwinclip,
    dependencies: dependency('xcb'),
    link_with: xwin_clipboard,
    # gdiplus + ole32: image clipboard conversion (CF_DIB -> png/jpeg via GDI+)
    link_args: ['-lgdi32', '-lgdiplus', '-lole32', '-lpthread'],
    install: true,
)

xwinclip_man = configure_file(
    input: 'xwinclip.man',
    output: 'xwinclip.1',
    configuration: manpage_config,
)
install_man(xwinclip_man)
