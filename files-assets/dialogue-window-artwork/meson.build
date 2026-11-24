foreach artwork : ['top-left-diagonals.svg', 'bottom-right-contour.svg']
  files_assets += [files_asset_copy.copyfile(artwork)]
  install_data(artwork,
    install_dir: get_option('datadir') / 'nythera' / 'files-assets' / 'dialogue-window-artwork',
    install_tag: 'files-assets')
endforeach
