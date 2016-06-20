Name:    freeradius
Version: %{__version}
Release: %{__release}%{?dist}

License: GNU AGPLv3
URL: https://github.com/redBorder/freeradius
Source0: %{name}-%{version}.tar.gz
i
BuildRequires: gcc make libtalloc-devel librdkafka-devel rbutils

Summary: High performance and highly configurable RADIUS server
Group: 
Requires: rbutils librdkafka

%description
%{summary}

%prep
%setup -qn %{name}-%{version}

%build
./configure --prefix=/usr
make

%install
DESTDIR=%{buildroot} make install
mkdir -p %{buildroot}/usr/share/%{name}
mkdir -p %{buildroot}/etc/%{name}
ln -sf %{buildroot}/usr/etc/raddb /etc/raddb 
install -D -m 644 freeradius.service %{buildroot}/usr/lib/systemd/system/freeradius.service

%clean
rm -rf %{buildroot}

%pre
getent group %{name} >/dev/null || groupadd -r %{name}
getent passwd %{name} >/dev/null || \
    useradd -r -g %{name} -d / -s /sbin/nologin \
    -c "User of %{name} service" %{name}
exit 0

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
%defattr(644,root,root)
/usr/include/%{name}
%defattr(755,root,root)
/usr/lib/libfreeradius*
%defattr(644,root,root)
/usr/share/doc/%{name}
%defattr(644,root,root)
/usr/share/%{name}
%defattr(755,root,root)
/usr/sbin/checkrad
/usr/sbin/raddebug
/usr/sbin/radiusd
/usr/sbin/radmin
/usr/sbin/radwatch
/usr/sbin/rc.radiusd
%defattr(755,root,root)
/usr/bin/radclient
/usr/bin/radconf2xml
/usr/bin/rad_counter
/usr/bin/radcrypt
/usr/bin/radeapclient
/usr/bin/radlast
/usr/bin/radsqlrelay
/usr/bin/radtest
/usr/bin/radwho
/usr/bin/radzap
/usr/bin/smbencrypt
%defattr(644,root,root)
/usr/etc/raddb
/etc/raddb
%defattr(644,root,root)
/usr/lib/systemd/system/%{name}.service

%changelog
* Fri Jun 10 2016 Alberto Rodriguez <arodriguez@redborder.com> - 1.0.18-1
- first spec version
