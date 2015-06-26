
logrotate_conf = groonga-server-gqtp groonga-server-http groonga-httpd

if INSTALL_INIT_FILES
logrotatedir = $(sysconfdir)/logrotate.d
dist_logrotate_DATA = $(logrotate_conf)
else
EXTRA_DIST = $(logrotate_conf)
endif
