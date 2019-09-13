i18n.merge_file ('desktop',
                 input: 'org.gnome.Dictionary.desktop.in',
                 output: 'org.gnome.Dictionary.desktop',
                 install: true,
                 install_dir: join_paths(gdict_datadir, 'applications'),
                 po_dir: po_dir,
                 type: 'desktop')

desktop_file_validate = find_program('desktop-file-validate', required: false)
if desktop_file_validate.found()
  test('validate-desktop',
       desktop_file_validate,
       args: 'org.gnome.Dictionary.desktop',
       workdir: meson.current_build_dir())
endif

i18n.merge_file ('appdata',
                 input: 'org.gnome.Dictionary.appdata.xml.in',
                 output: 'org.gnome.Dictionary.appdata.xml',
                 install: true,
                 install_dir: join_paths(gdict_datadir, 'metainfo'),
                 po_dir: po_dir)

appstream_util = find_program('appstream-util', required: false)
if appstream_util.found()
  test('validate-appdata',
       appstream_util,
       args: [ 'validate', '--nonet', 'org.gnome.Dictionary.appdata.xml' ],
       workdir: meson.current_build_dir())
endif

service_conf = configuration_data ()
service_conf.set ('bindir', gdict_bindir)
configure_file (input: 'org.gnome.Dictionary.service.in',
                output: 'org.gnome.Dictionary.service',
                configuration: service_conf,
                install_dir: gdict_servicedir)

install_data ('org.gnome.dictionary.gschema.xml',
              install_dir: gdict_schemadir)


# Dictionary sources
sources = [
  'default.desktop',
  'thai.desktop',
]

foreach s: sources
  i18n.merge_file('sources',
                  input: '@0@.in'.format(s),
                  output: s,
                  install: true,
                  install_dir: join_paths(gdict_datadir, 'gdict-1.0', 'sources'),
                  po_dir: po_dir,
                  type: 'desktop')
endforeach

xsltproc = find_program('xsltproc', required: false)

if get_option('build_man') and xsltproc.found()
  xlstproc_flags = [
    '--nonet',
    '--stringparam', 'man.output.quietly', '1',
    '--stringparam', 'funcsynopsis.style', 'ansi',
    '--stringparam', 'man.th.extra1.suppress', '1',
    '--stringparam', 'man.authors.section.enabled', '1',
    '--stringparam', 'man.copyright.section.enabled', '1',
  ]

  custom_target('man pages',
                input: 'gnome-dictionary.xml',
                output: 'gnome-dictionary.1',
                command: [
                  xsltproc,
                  xlstproc_flags,
                  '-o', '@OUTPUT@',
                  'http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl',
                  '@INPUT@',
                ],
                install: true,
                install_dir: join_paths(gdict_mandir, 'man1'))
endif
