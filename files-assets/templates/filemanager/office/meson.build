foreach office_template : ['blank.odt', 'blank.ods', 'blank.odp']
  files_assets += [files_asset_copy.copyfile(office_template)]
  install_data(office_template,
    install_dir: get_option('datadir') / 'nythera' / 'files-assets' / 'templates' / 'filemanager' / 'office',
    install_tag: 'files-assets')
endforeach
