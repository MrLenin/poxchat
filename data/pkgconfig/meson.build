pkg_conf = configuration_data()
prefix = get_option('prefix')
pkg_conf.set('prefix', prefix)
pkg_conf.set('VERSION', meson.project_version())
pkg_conf.set('poxchatlibdir', join_paths('${prefix}', plugindir))
pkg_conf.set('includedir', join_paths('${prefix}', get_option('includedir')))

configure_file(
  input: 'poxchat-plugin.pc.in',
  output: 'poxchat-plugin.pc',
  configuration: pkg_conf,
  install: true,
  install_dir: join_paths(get_option('libdir'), 'pkgconfig'),
)
