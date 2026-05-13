icondir = join_paths(get_option('datadir'), 'icons/hicolor')

foreach size : ['48', '64', '128', '256']
  install_data(
    'poxchat-' + size + '.png',
    rename: 'io.github.evilnet.PoxChat.png',
    install_dir: join_paths(icondir, size + 'x' + size, 'apps')
  )
endforeach
