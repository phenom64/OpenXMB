"""Finder layout for the OpenXMB drag-to-install disk image."""
app = defines['app']
files = [app]
symlinks = {'Applications': '/Applications'}
background = defines['background']
format = 'UDZO'
window_rect = ((160, 120), (900, 600))
icon_locations = {'OpenXMB.app': (250, 320), 'Applications': (650, 320)}
icon_size = 112
text_size = 16
show_status_bar = False
show_tab_view = False
show_toolbar = False
show_pathbar = False
show_sidebar = False
default_view = 'icon-view'
include_icon_view_settings = True
include_list_view_settings = False
