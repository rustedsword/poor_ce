install_data('ce.desktop', install_dir: get_option('datadir') / 'applications')
install_data('ce.svg', install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps')

gnome = import('gnome')
gnome.post_install(
	gtk_update_icon_cache: find_program('gtk-update-icon-cache', required: false).found(),
	update_desktop_database: find_program('update-desktop-database', required: false).found(),
)
