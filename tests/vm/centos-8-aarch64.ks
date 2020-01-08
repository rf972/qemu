#version=RHEL8
ignoredisk --only-use=sda
# System bootloader configuration
bootloader --append=" crashkernel=auto" --location=mbr --boot-drive=sda
autopart --type=plain
# Partition clearing information
clearpart --linux --initlabel --drives=sda
# Use text mode install
text
repo --name="AppStream" --baseurl=file:///run/install/repo/AppStream
# Use CDROM installation media
cdrom
# Keyboard layouts
keyboard --vckeymap=us --xlayouts=''
# System language
lang en_US.UTF-8

# Network information
network  --bootproto=dhcp --device=enp0s1 --onboot=off --ipv6=auto --no-activate
network  --hostname=localhost.localdomain
# Root password
rootpw --iscrypted $6$5p4XDxL5q4BnQFAz$AZwEhzUbJFzjcCLTGFNSSbwh9ZquCgQnbXx/q7P2FgSJiMfxSSEBZVZkC57KpB0Lw9QcspLQvKozwQGfm3khr.
# Run the Setup Agent on first boot
firstboot --enable
# Do not configure the X Window System
skipx
# System services
services --enabled="chronyd"
# System timezone
timezone America/New_York --isUtc
user --groups=wheel --name=qemu --password=$6$vOog88JONdf6teTb$rTk9HI.vzLHLhmB8gAlH3jesiaQn6DU24P08tqLo9AyqIdxBR9QvjI5Wtwzz09MwoRsbh3De743gaLQ2x9vhe. --iscrypted

# Shutdown after installation is complete.
shutdown

%packages
@^server-product-environment
kexec-tools

%end

%addon com_redhat_kdump --enable --reserve-mb='auto'

%end
%anaconda
pwpolicy root --minlen=6 --minquality=1 --notstrict --nochanges --notempty
pwpolicy user --minlen=6 --minquality=1 --notstrict --nochanges --emptyok
pwpolicy luks --minlen=6 --minquality=1 --notstrict --nochanges --notempty
%end

