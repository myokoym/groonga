data = groonga-server-http groonga-server-gqtp

if INSTALL_SYSTEMD_FILES
sysconfigdir = $(sysconfdir)/sysconfig
dist_sysconfig_DATA = $(data)
else
EXTRA_DIST = $(data)
endif
