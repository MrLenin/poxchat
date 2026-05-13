dbus_deps = [
  dbus_glib_dep
]

dbus_sources = [
  'dbus-plugin.c',
  'dbus-client.c'
]

dbus_includes = [
 include_directories('..'),
 config_h_include
]

dbus_service_dir = join_paths(get_option('datadir'), 'dbus-1/services')

if get_option('dbus-service-use-appid')
  dbus_service_name = 'io.github.evilnet.PoxChat'
  dbus_output_file = 'io.github.evilnet.PoxChat.service'
  dbus_cargs = '-DDBUS_SERVICE="io.github.evilnet.PoxChat"'
else
  dbus_service_name = 'org.poxchat.service'
  dbus_output_file = 'org.poxchat.service.service'
  dbus_cargs = '-DDBUS_SERVICE="org.poxchat.service"'
endif

dbus_conf = configuration_data()
dbus_conf.set('bindir', join_paths(get_option('prefix'), get_option('bindir')))
dbus_conf.set('service_name', dbus_service_name)
configure_file(
  input: 'org.poxchat.service.service.in',
  output: dbus_output_file,
  configuration: dbus_conf,
  install: true,
  install_dir: dbus_service_dir
)

dbus_binding_tool = find_program('dbus-binding-tool')
dbus_remote_object = custom_target('remote-object-glue',
  input: 'remote-object.xml',
  output: 'remote-object-glue.h',
  command: [dbus_binding_tool, '--prefix=remote_object', '--mode=glib-server',
            '--output=@OUTPUT@', '@INPUT@']
)

poxchat_dbus = static_library('poxchatdbus',
  sources: [dbus_remote_object, marshal] + dbus_sources,
  c_args: dbus_cargs,
  dependencies: common_deps + dbus_deps,
  include_directories: dbus_includes,
  pic: true
)

poxchat_dbus_dep = declare_dependency(
  link_with: poxchat_dbus
)
