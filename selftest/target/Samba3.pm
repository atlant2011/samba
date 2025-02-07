#!/usr/bin/perl
# Bootstrap Samba and run a number of tests against it.
# Copyright (C) 2005-2007 Jelmer Vernooij <jelmer@samba.org>
# Published under the GNU GPL, v3 or later.

package Samba3;

use strict;
use Cwd qw(abs_path);
use FindBin qw($RealBin);
use POSIX;
use target::Samba;

sub new($$) {
	my ($classname, $bindir, $binary_mapping, $srcdir, $exeext, $server_maxtime) = @_;
	$exeext = "" unless defined($exeext);
	my $self = { vars => {},
		     bindir => $bindir,
		     binary_mapping => $binary_mapping,
		     srcdir => $srcdir,
		     exeext => $exeext,
		     server_maxtime => $server_maxtime
	};
	bless $self;
	return $self;
}

sub teardown_env($$)
{
	my ($self, $envvars) = @_;

	my $smbdpid = read_pid($envvars, "smbd");
	my $nmbdpid = read_pid($envvars, "nmbd");
	my $winbinddpid = read_pid($envvars, "winbindd");

	$self->stop_sig_term($smbdpid);
	$self->stop_sig_term($nmbdpid);
	$self->stop_sig_term($winbinddpid);

	sleep(2);

	$self->stop_sig_kill($smbdpid);
	$self->stop_sig_kill($nmbdpid);
	$self->stop_sig_kill($winbinddpid);

	return 0;
}

sub getlog_env_app($$$)
{
	my ($self, $envvars, $name) = @_;

	my $title = "$name LOG of: $envvars->{NETBIOSNAME}\n";
	my $out = $title;

	open(LOG, "<".$envvars->{$name."_TEST_LOG"});

	seek(LOG, $envvars->{$name."_TEST_LOG_POS"}, SEEK_SET);
	while (<LOG>) {
		$out .= $_;
	}
	$envvars->{$name."_TEST_LOG_POS"} = tell(LOG);
	close(LOG);

	return "" if $out eq $title;
 
	return $out;
}

sub getlog_env($$)
{
	my ($self, $envvars) = @_;
	my $ret = "";

	$ret .= $self->getlog_env_app($envvars, "SMBD");
	$ret .= $self->getlog_env_app($envvars, "NMBD");
	$ret .= $self->getlog_env_app($envvars, "WINBINDD");

	return $ret;
}

sub check_env($$)
{
	my ($self, $envvars) = @_;

	# TODO ...
	return 1;
}

sub setup_env($$$)
{
	my ($self, $envname, $path) = @_;
	
	if ($envname eq "s3dc") {
		return $self->setup_s3dc("$path/s3dc");
	} elsif ($envname eq "secshare") {
		return $self->setup_secshare("$path/secshare");
	} elsif ($envname eq "maptoguest") {
		return $self->setup_maptoguest("$path/maptoguest");
	} elsif ($envname eq "ktest") {
		return $self->setup_ktest("$path/ktest");
	} elsif ($envname eq "secserver") {
		if (not defined($self->{vars}->{s3dc})) {
			if (not defined($self->setup_s3dc("$path/s3dc"))) {
			        return undef;
			}
		}
		return $self->setup_secserver("$path/secserver", $self->{vars}->{s3dc});
	} elsif ($envname eq "member") {
		if (not defined($self->{vars}->{s3dc})) {
			if (not defined($self->setup_s3dc("$path/s3dc"))) {
			        return undef;
			}
		}
		return $self->setup_member("$path/member", $self->{vars}->{s3dc});
	} else {
		return undef;
	}
}

sub setup_s3dc($$)
{
	my ($self, $path) = @_;

	print "PROVISIONING S3DC...";

	my $s3dc_options = "
	domain master = yes
	domain logons = yes
	lanman auth = yes
";

	my $vars = $self->provision($path,
				    "LOCALS3DC2",
				    2,
				    "locals3dc2pass",
				    $s3dc_options);

	$vars or return undef;

	$self->check_or_start($vars,
			       "yes", "yes", "yes");

	if (not $self->wait_for_start($vars)) {
	       return undef;
	}

	$vars->{DC_SERVER} = $vars->{SERVER};
	$vars->{DC_SERVER_IP} = $vars->{SERVER_IP};
	$vars->{DC_NETBIOSNAME} = $vars->{NETBIOSNAME};
	$vars->{DC_USERNAME} = $vars->{USERNAME};
	$vars->{DC_PASSWORD} = $vars->{PASSWORD};

	$self->{vars}->{s3dc} = $vars;

	return $vars;
}

sub setup_member($$$)
{
	my ($self, $prefix, $s3dcvars) = @_;

	print "PROVISIONING MEMBER...";

	my $member_options = "
	security = domain
	server signing = on
";
	my $ret = $self->provision($prefix,
				   "LOCALMEMBER3",
				   3,
				   "localmember3pass",
				   $member_options);

	$ret or return undef;

	my $net = Samba::bindir_path($self, "net");
	my $cmd = "";
	$cmd .= "SOCKET_WRAPPER_DEFAULT_IFACE=\"$ret->{SOCKET_WRAPPER_DEFAULT_IFACE}\" ";
	$cmd .= "$net join $ret->{CONFIGURATION} $s3dcvars->{DOMAIN} member";
	$cmd .= " -U$s3dcvars->{USERNAME}\%$s3dcvars->{PASSWORD}";

	if (system($cmd) != 0) {
	    warn("Join failed\n$cmd");
	    return undef;
	}

	$self->check_or_start($ret, "yes", "yes", "yes");

	if (not $self->wait_for_start($ret)) {
	       return undef;
	}

	$ret->{DC_SERVER} = $s3dcvars->{SERVER};
	$ret->{DC_SERVER_IP} = $s3dcvars->{SERVER_IP};
	$ret->{DC_NETBIOSNAME} = $s3dcvars->{NETBIOSNAME};
	$ret->{DC_USERNAME} = $s3dcvars->{USERNAME};
	$ret->{DC_PASSWORD} = $s3dcvars->{PASSWORD};

	return $ret;
}

sub setup_admember($$$$)
{
	my ($self, $prefix, $dcvars, $iface) = @_;

	print "PROVISIONING S3 AD MEMBER$iface...";

	my $member_options = "
	security = ads
	server signing = on
        workgroup = $dcvars->{DOMAIN}
        realm = $dcvars->{REALM}
";

	my $ret = $self->provision($prefix,
				   "LOCALADMEMBER$iface",
				   $iface,
				   "loCalMember${iface}Pass",
				   $member_options);

	$ret or return undef;

	close(USERMAP);
	$ret->{DOMAIN} = $dcvars->{DOMAIN};
	$ret->{REALM} = $dcvars->{REALM};

	my $ctx;
	my $prefix_abs = abs_path($prefix);
	$ctx = {};
	$ctx->{krb5_conf} = "$prefix_abs/lib/krb5.conf";
	$ctx->{domain} = $dcvars->{DOMAIN};
	$ctx->{realm} = $dcvars->{REALM};
	$ctx->{dnsname} = lc($dcvars->{REALM});
	$ctx->{kdc_ipv4} = $dcvars->{SERVER_IP};
	Samba::mk_krb5_conf($ctx, "");

	$ret->{KRB5_CONFIG} = $ctx->{krb5_conf};

	my $net = Samba::bindir_path($self, "net");
	my $cmd = "";
	$cmd .= "SOCKET_WRAPPER_DEFAULT_IFACE=\"$ret->{SOCKET_WRAPPER_DEFAULT_IFACE}\" ";
	$cmd .= "KRB5_CONFIG=\"$ret->{KRB5_CONFIG}\" ";
	$cmd .= "$net join $ret->{CONFIGURATION}";
	$cmd .= " -U$dcvars->{USERNAME}\%$dcvars->{PASSWORD}";

	if (system($cmd) != 0) {
	    warn("Join failed\n$cmd");
	    return undef;
	}

	# We need world access to this share, as otherwise the domain
	# administrator from the AD domain provided by Samba4 can't
	# access the share for tests.
	chmod 0777, "$prefix/share";

	$self->check_or_start($ret,
			      "yes", "yes", "yes");

	$self->wait_for_start($ret);

	$ret->{DC_SERVER} = $dcvars->{SERVER};
	$ret->{DC_SERVER_IP} = $dcvars->{SERVER_IP};
	$ret->{DC_NETBIOSNAME} = $dcvars->{NETBIOSNAME};
	$ret->{DC_USERNAME} = $dcvars->{USERNAME};
	$ret->{DC_PASSWORD} = $dcvars->{PASSWORD};

	# Special case, this is called from Samba4.pm but needs to use the Samba3 check_env and get_log_env
	$ret->{target} = $self;

	return $ret;
}

sub setup_plugin_s4_dc($$$$)
{
	my ($self, $prefix, $dcvars, $iface) = @_;

	print "PROVISIONING S4 PLUGIN AD DC$iface...";

	my $plugin_s4_dc_options = "
        workgroup = $dcvars->{DOMAIN}
        realm = $dcvars->{REALM}
        security=ads
        passdb backend = samba4
        auth methods = guest samba4
        domain logons = yes
        rpc_server:epmapper = external
        rpc_daemon:epmd = disabled
        rpc_daemon:lsasd = disabled
        rpc_server:tcpip = no
        rpc_server:lsarpc = external
        rpc_server:netlogon = external
        rpc_server:samr = external
	server signing = on
";

	my $ret = $self->provision($prefix,
				   "plugindc",
				   $iface,
				   "pluGin${iface}Pass",
				   $plugin_s4_dc_options, 1);

	$ret or return undef;

	close(USERMAP);
	$ret->{DOMAIN} = $dcvars->{DOMAIN};
	$ret->{REALM} = $dcvars->{REALM};
	$ret->{KRB5_CONFIG} = $dcvars->{KRB5_CONFIG};

	# We need world access to this share, as otherwise the domain
	# administrator from the AD domain provided by Samba4 can't
	# access the share for tests.
	chmod 0777, "$prefix/share";

	$self->check_or_start($ret,
			      "no", "no", "yes");

	$self->wait_for_start($ret);

	# Special case, this is called from Samba4.pm but needs to use the Samba3 check_env and get_log_env
	$ret->{target} = $self;

	return $ret;
}

sub setup_secshare($$)
{
	my ($self, $path) = @_;

	print "PROVISIONING server with security=share...";

	my $secshare_options = "
	security = share
	lanman auth = yes
";

	my $vars = $self->provision($path,
				    "LOCALSHARE4",
				    4,
				    "local4pass",
				    $secshare_options);

	$vars or return undef;

	$self->check_or_start($vars, "yes", "no", "yes");

	if (not $self->wait_for_start($vars)) {
	       return undef;
	}

	$self->{vars}->{secshare} = $vars;

	return $vars;
}

sub setup_secserver($$$)
{
	my ($self, $prefix, $s3dcvars) = @_;

	print "PROVISIONING server with security=server...";

	my $secserver_options = "
	security = server
        password server = $s3dcvars->{SERVER_IP}
";

	my $ret = $self->provision($prefix,
				   "LOCALSERVER5",
				   5,
				   "localserver5pass",
				   $secserver_options);

	$ret or return undef;

	$self->check_or_start($ret, "yes", "no", "yes");

	if (not $self->wait_for_start($ret)) {
	       return undef;
	}

	$ret->{DC_SERVER} = $s3dcvars->{SERVER};
	$ret->{DC_SERVER_IP} = $s3dcvars->{SERVER_IP};
	$ret->{DC_NETBIOSNAME} = $s3dcvars->{NETBIOSNAME};
	$ret->{DC_USERNAME} = $s3dcvars->{USERNAME};
	$ret->{DC_PASSWORD} = $s3dcvars->{PASSWORD};

	return $ret;
}

sub setup_ktest($$$)
{
	my ($self, $prefix) = @_;

	print "PROVISIONING server with security=ads...";

	my $ktest_options = "
        workgroup = KTEST
        realm = ktest.samba.example.com
	security = ads
        username map = $prefix/lib/username.map
";

	my $ret = $self->provision($prefix,
				   "LOCALKTEST6",
				   6,
				   "localktest6pass",
				   $ktest_options);

	$ret or return undef;

	my $ctx;
	my $prefix_abs = abs_path($prefix);
	$ctx = {};
	$ctx->{krb5_conf} = "$prefix_abs/lib/krb5.conf";
	$ctx->{domain} = "KTEST";
	$ctx->{realm} = "KTEST.SAMBA.EXAMPLE.COM";
	$ctx->{dnsname} = lc($ctx->{realm});
	$ctx->{kdc_ipv4} = "0.0.0.0";
	Samba::mk_krb5_conf($ctx, "");

	$ret->{KRB5_CONFIG} = $ctx->{krb5_conf};

	open(USERMAP, ">$prefix/lib/username.map") or die("Unable to open $prefix/lib/username.map");
	print USERMAP "
$ret->{USERNAME} = KTEST\\Administrator
";
	close(USERMAP);

#This is the secrets.tdb created by 'net ads join' from Samba3 to a
#Samba4 DC with the same parameters as are being used here.  The
#domain SID is S-1-5-21-1071277805-689288055-3486227160

	if (defined($ENV{BUILD_TDB2})) {
	    system("cp $self->{srcdir}/source3/selftest/ktest-secrets.tdb2 $prefix/private/secrets.tdb");
	} else {
	    system("cp $self->{srcdir}/source3/selftest/ktest-secrets.tdb $prefix/private/secrets.tdb");
	}
	chmod 0600, "$prefix/private/secrets.tdb";

#This uses a pre-calculated krb5 credentials cache, obtained by running Samba4 with:
# "--option=kdc:service ticket lifetime=239232" "--option=kdc:user ticket lifetime=239232" "--option=kdc:renewal lifetime=239232"
#
#and having in krb5.conf:
# ticket_lifetime = 799718400
# renew_lifetime = 799718400
#
# The commands for the -2 keytab where were:
# kinit administrator@KTEST.SAMBA.EXAMPLE.COM
# kvno host/localktest6@KTEST.SAMBA.EXAMPLE.COM
# kvno cifs/localktest6@KTEST.SAMBA.EXAMPLE.COM
# kvno host/LOCALKTEST6@KTEST.SAMBA.EXAMPLE.COM
# kvno cifs/LOCALKTEST6@KTEST.SAMBA.EXAMPLE.COM
#
# and then for the -3 keytab, I did
#
# net changetrustpw; kdestroy and the same again.
#
# This creates a credential cache with a very long lifetime (2036 at
# at 2011-04), and shows that running 'net changetrustpw' does not
# break existing logins (for the secrets.tdb method at least).
#

	$ret->{KRB5_CCACHE}="FILE:$prefix/krb5_ccache";

	system("cp $self->{srcdir}/source3/selftest/ktest-krb5_ccache-2 $prefix/krb5_ccache-2");
	chmod 0600, "$prefix/krb5_ccache-2";

	system("cp $self->{srcdir}/source3/selftest/ktest-krb5_ccache-3 $prefix/krb5_ccache-3");
	chmod 0600, "$prefix/krb5_ccache-3";

	$self->check_or_start($ret, "yes", "no", "yes");

	if (not $self->wait_for_start($ret)) {
	       return undef;
	}
	return $ret;
}

sub setup_maptoguest($$)
{
	my ($self, $path) = @_;

	print "PROVISIONING maptoguest...";

	my $options = "
map to guest = bad user
";

	my $vars = $self->provision($path,
				    "maptoguest",
				    7,
				    "maptoguestpass",
				    $options);

	$vars or return undef;

	$self->check_or_start($vars,
			       "yes", "no", "yes");

	if (not $self->wait_for_start($vars)) {
	       return undef;
	}

	$self->{vars}->{s3maptoguest} = $vars;

	return $vars;
}

sub stop_sig_term($$) {
	my ($self, $pid) = @_;
	kill("USR1", $pid) or kill("ALRM", $pid) or warn("Unable to kill $pid: $!");
}

sub stop_sig_kill($$) {
	my ($self, $pid) = @_;
	kill("ALRM", $pid) or warn("Unable to kill $pid: $!");
}

sub write_pid($$$)
{
	my ($env_vars, $app, $pid) = @_;

	open(PID, ">$env_vars->{PIDDIR}/timelimit.$app.pid");
	print PID $pid;
	close(PID);
}

sub read_pid($$)
{
	my ($env_vars, $app) = @_;

	open(PID, "<$env_vars->{PIDDIR}/timelimit.$app.pid");
	my $pid = <PID>;
	close(PID);
	return $pid;
}

sub check_or_start($$$$) {
	my ($self, $env_vars, $nmbd, $winbindd, $smbd) = @_;

	unlink($env_vars->{NMBD_TEST_LOG});
	print "STARTING NMBD...";
	my $pid = fork();
	if ($pid == 0) {
		open STDOUT, ">$env_vars->{NMBD_TEST_LOG}";
		open STDERR, '>&STDOUT';

		SocketWrapper::set_default_iface($env_vars->{SOCKET_WRAPPER_DEFAULT_IFACE});

		$ENV{KRB5_CONFIG} = $env_vars->{KRB5_CONFIG};
		$ENV{WINBINDD_SOCKET_DIR} = $env_vars->{WINBINDD_SOCKET_DIR};
		$ENV{NMBD_SOCKET_DIR} = $env_vars->{NMBD_SOCKET_DIR};

		$ENV{NSS_WRAPPER_PASSWD} = $env_vars->{NSS_WRAPPER_PASSWD};
		$ENV{NSS_WRAPPER_GROUP} = $env_vars->{NSS_WRAPPER_GROUP};
		$ENV{NSS_WRAPPER_WINBIND_SO_PATH} = $env_vars->{NSS_WRAPPER_WINBIND_SO_PATH};

		if ($nmbd ne "yes") {
			$SIG{USR1} = $SIG{ALRM} = $SIG{INT} = $SIG{QUIT} = $SIG{TERM} = sub {
				my $signame = shift;
				print("Skip nmbd received signal $signame");
				exit 0;
			};
			sleep($self->{server_maxtime});
			exit 0;
		}

		my @optargs = ("-d0");
		if (defined($ENV{NMBD_OPTIONS})) {
			@optargs = split(/ /, $ENV{NMBD_OPTIONS});
		}

		$ENV{MAKE_TEST_BINARY} = Samba::bindir_path($self, "nmbd");

		my @preargs = (Samba::bindir_path($self, "timelimit"), $self->{server_maxtime});
		if(defined($ENV{NMBD_VALGRIND})) { 
			@preargs = split(/ /, $ENV{NMBD_VALGRIND});
		}

		exec(@preargs, Samba::bindir_path($self, "nmbd"), "-F", "--no-process-group", "--log-stdout", "-s", $env_vars->{SERVERCONFFILE}, @optargs) or die("Unable to start nmbd: $!");
	}
	write_pid($env_vars, "nmbd", $pid);
	print "DONE\n";

	unlink($env_vars->{WINBINDD_TEST_LOG});
	print "STARTING WINBINDD...";
	$pid = fork();
	if ($pid == 0) {
		open STDOUT, ">$env_vars->{WINBINDD_TEST_LOG}";
		open STDERR, '>&STDOUT';

		SocketWrapper::set_default_iface($env_vars->{SOCKET_WRAPPER_DEFAULT_IFACE});

		$ENV{KRB5_CONFIG} = $env_vars->{KRB5_CONFIG};
		$ENV{WINBINDD_SOCKET_DIR} = $env_vars->{WINBINDD_SOCKET_DIR};
		$ENV{NMBD_SOCKET_DIR} = $env_vars->{NMBD_SOCKET_DIR};

		$ENV{NSS_WRAPPER_PASSWD} = $env_vars->{NSS_WRAPPER_PASSWD};
		$ENV{NSS_WRAPPER_GROUP} = $env_vars->{NSS_WRAPPER_GROUP};
		$ENV{NSS_WRAPPER_WINBIND_SO_PATH} = $env_vars->{NSS_WRAPPER_WINBIND_SO_PATH};

		if ($winbindd ne "yes") {
			$SIG{USR1} = $SIG{ALRM} = $SIG{INT} = $SIG{QUIT} = $SIG{TERM} = sub {
				my $signame = shift;
				print("Skip winbindd received signal $signame");
				exit 0;
			};
			sleep($self->{server_maxtime});
			exit 0;
		}

		my @optargs = ("-d0");
		if (defined($ENV{WINBINDD_OPTIONS})) {
			@optargs = split(/ /, $ENV{WINBINDD_OPTIONS});
		}

		$ENV{MAKE_TEST_BINARY} = Samba::bindir_path($self, "winbindd");

		my @preargs = (Samba::bindir_path($self, "timelimit"), $self->{server_maxtime});
		if(defined($ENV{WINBINDD_VALGRIND})) {
			@preargs = split(/ /, $ENV{WINBINDD_VALGRIND});
		}

		print "Starting winbindd with config $env_vars->{SERVERCONFFILE})\n";

		exec(@preargs, Samba::bindir_path($self, "winbindd"), "-F", "--no-process-group", "--stdout", "-s", $env_vars->{SERVERCONFFILE}, @optargs) or die("Unable to start winbindd: $!");
	}
	write_pid($env_vars, "winbindd", $pid);
	print "DONE\n";

	unlink($env_vars->{SMBD_TEST_LOG});
	print "STARTING SMBD...";
	$pid = fork();
	if ($pid == 0) {
		open STDOUT, ">$env_vars->{SMBD_TEST_LOG}";
		open STDERR, '>&STDOUT';

		SocketWrapper::set_default_iface($env_vars->{SOCKET_WRAPPER_DEFAULT_IFACE});

		$ENV{KRB5_CONFIG} = $env_vars->{KRB5_CONFIG};
		$ENV{WINBINDD_SOCKET_DIR} = $env_vars->{WINBINDD_SOCKET_DIR};
		$ENV{NMBD_SOCKET_DIR} = $env_vars->{NMBD_SOCKET_DIR};

		$ENV{NSS_WRAPPER_PASSWD} = $env_vars->{NSS_WRAPPER_PASSWD};
		$ENV{NSS_WRAPPER_GROUP} = $env_vars->{NSS_WRAPPER_GROUP};
		$ENV{NSS_WRAPPER_WINBIND_SO_PATH} = $env_vars->{NSS_WRAPPER_WINBIND_SO_PATH};

		if ($smbd ne "yes") {
			$SIG{USR1} = $SIG{ALRM} = $SIG{INT} = $SIG{QUIT} = $SIG{TERM} = sub {
				my $signame = shift;
				print("Skip smbd received signal $signame");
				exit 0;
			};
			sleep($self->{server_maxtime});
			exit 0;
		}

		$ENV{MAKE_TEST_BINARY} = Samba::bindir_path($self, "smbd");
		my @optargs = ("-d0");
		if (defined($ENV{SMBD_OPTIONS})) {
			@optargs = split(/ /, $ENV{SMBD_OPTIONS});
		}
		my @preargs = (Samba::bindir_path($self, "timelimit"), $self->{server_maxtime});
		if(defined($ENV{SMBD_VALGRIND})) {
			@preargs = split(/ /,$ENV{SMBD_VALGRIND});
		}
		exec(@preargs, Samba::bindir_path($self, "smbd"), "-F", "--no-process-group", "--log-stdout", "-s", $env_vars->{SERVERCONFFILE}, @optargs) or die("Unable to start smbd: $!");
	}
	write_pid($env_vars, "smbd", $pid);
	print "DONE\n";

	return 0;
}

sub provision($$$$$$$)
{
	my ($self, $prefix, $server, $swiface, $password, $extra_options, $no_delete_prefix) = @_;

	##
	## setup the various environment variables we need
	##

	my %ret = ();
	my $server_ip = "127.0.0.$swiface";
	my $domain = "SAMBA-TEST";

	my $unix_name = ($ENV{USER} or $ENV{LOGNAME} or `PATH=/usr/ucb:$ENV{PATH} whoami`);
	chomp $unix_name;
	my $unix_uid = $>;
	my $unix_gids_str = $);
	my @unix_gids = split(" ", $unix_gids_str);

	my $prefix_abs = abs_path($prefix);
	my $bindir_abs = abs_path($self->{bindir});
	my $vfs_modulesdir_abs = ($ENV{VFSLIBDIR} or $bindir_abs);

	my $dns_host_file = "$ENV{SELFTEST_PREFIX}/dns_host_file";

	my @dirs = ();

	my $shrdir="$prefix_abs/share";
	push(@dirs,$shrdir);

	my $libdir="$prefix_abs/lib";
	push(@dirs,$libdir);

	my $piddir="$prefix_abs/pid";
	push(@dirs,$piddir);

	my $privatedir="$prefix_abs/private";
	push(@dirs,$privatedir);

	my $lockdir="$prefix_abs/lockdir";
	push(@dirs,$lockdir);

	my $eventlogdir="$prefix_abs/lockdir/eventlog";
	push(@dirs,$eventlogdir);

	my $logdir="$prefix_abs/logs";
	push(@dirs,$logdir);

	my $driver32dir="$shrdir/W32X86";
	push(@dirs,$driver32dir);

	my $driver64dir="$shrdir/x64";
	push(@dirs,$driver64dir);

	my $driver40dir="$shrdir/WIN40";
	push(@dirs,$driver40dir);

	my $ro_shrdir="$shrdir/root-tmp";
	push(@dirs,$ro_shrdir);

	my $msdfs_shrdir="$shrdir/msdfsshare";
	push(@dirs,$msdfs_shrdir);

	my $msdfs_deeppath="$msdfs_shrdir/deeppath";
	push(@dirs,$msdfs_deeppath);

	# this gets autocreated by winbindd
	my $wbsockdir="$prefix_abs/winbindd";
	my $wbsockprivdir="$lockdir/winbindd_privileged";

	my $nmbdsockdir="$prefix_abs/nmbd";
	unlink($nmbdsockdir);

	## 
	## create the test directory layout
	##
	die ("prefix_abs = ''") if $prefix_abs eq "";
	die ("prefix_abs = '/'") if $prefix_abs eq "/";

	mkdir($prefix_abs, 0777);
	print "CREATE TEST ENVIRONMENT IN '$prefix'...";
	if (not defined($no_delete_prefix) or not $no_delete_prefix) {
	    system("rm -rf $prefix_abs/*");
	}
	mkdir($_, 0777) foreach(@dirs);

	##
	## create ro and msdfs share layout
	##

	chmod 0755, $ro_shrdir;
	my $unreadable_file = "$ro_shrdir/unreadable_file";
	unless (open(UNREADABLE_FILE, ">$unreadable_file")) {
	        warn("Unable to open $unreadable_file");
		return undef;
	}
	close(UNREADABLE_FILE);
	chmod 0600, $unreadable_file;

	my $msdfs_target = "$ro_shrdir/msdfs-target";
	unless (open(MSDFS_TARGET, ">$msdfs_target")) {
	        warn("Unable to open $msdfs_target");
		return undef;
	}
	close(MSDFS_TARGET);
	chmod 0666, $msdfs_target;
	symlink "msdfs:$server_ip\\ro-tmp", "$msdfs_shrdir/msdfs-src1";
	symlink "msdfs:$server_ip\\ro-tmp", "$msdfs_shrdir/deeppath/msdfs-src2";

	my $conffile="$libdir/server.conf";

	my $nss_wrapper_pl = "$ENV{PERL} $self->{srcdir}/lib/nss_wrapper/nss_wrapper.pl";
	my $nss_wrapper_passwd = "$privatedir/passwd";
	my $nss_wrapper_group = "$privatedir/group";

	my $mod_printer_pl = "$ENV{PERL} $self->{srcdir}/source3/script/tests/printing/modprinter.pl";

	my @eventlog_list = ("dns server", "application");

	##
	## calculate uids and gids
	##

	my ($max_uid, $max_gid);
	my ($uid_nobody, $uid_root);
	my ($gid_nobody, $gid_nogroup, $gid_root, $gid_domusers);

	if ($unix_uid < 0xffff - 2) {
		$max_uid = 0xffff;
	} else {
		$max_uid = $unix_uid;
	}

	$uid_root = $max_uid - 1;
	$uid_nobody = $max_uid - 2;

	if ($unix_gids[0] < 0xffff - 3) {
		$max_gid = 0xffff;
	} else {
		$max_gid = $unix_gids[0];
	}

	$gid_nobody = $max_gid - 1;
	$gid_nogroup = $max_gid - 2;
	$gid_root = $max_gid - 3;
	$gid_domusers = $max_gid - 4;

	##
	## create conffile
	##

	unless (open(CONF, ">$conffile")) {
	        warn("Unable to open $conffile");
		return undef;
	}
	print CONF "
[global]
	netbios name = $server
	interfaces = $server_ip/8
	bind interfaces only = yes
	panic action = $self->{srcdir}/selftest/gdb_backtrace %d %\$(MAKE_TEST_BINARY)

	workgroup = $domain

	private dir = $privatedir
	pid directory = $piddir
	lock directory = $lockdir
	log file = $logdir/log.\%m
	log level = 0
	debug pid = yes
        max log size = 0

	state directory = $lockdir
	cache directory = $lockdir

	passdb backend = tdbsam

	time server = yes

	add user script =		$nss_wrapper_pl --passwd_path $nss_wrapper_passwd --type passwd --action add --name %u --gid $gid_nogroup
	add group script =		$nss_wrapper_pl --group_path  $nss_wrapper_group  --type group  --action add --name %g
	add machine script =		$nss_wrapper_pl --passwd_path $nss_wrapper_passwd --type passwd --action add --name %u --gid $gid_nogroup
	add user to group script =	$nss_wrapper_pl --passwd_path $nss_wrapper_passwd --type member --action add --member %u --name %g --group_path $nss_wrapper_group
	delete user script =		$nss_wrapper_pl --passwd_path $nss_wrapper_passwd --type passwd --action delete --name %u
	delete group script =		$nss_wrapper_pl --group_path  $nss_wrapper_group  --type group  --action delete --name %g
	delete user from group script = $nss_wrapper_pl --passwd_path $nss_wrapper_passwd --type member --action delete --member %u --name %g --group_path $nss_wrapper_group

	addprinter command =		$mod_printer_pl -a -s $conffile --
	deleteprinter command =		$mod_printer_pl -d -s $conffile --

	eventlog list = application \"dns server\"

	kernel oplocks = no
	kernel change notify = no

	syslog = no
	printing = bsd
	printcap name = /dev/null

	winbindd:socket dir = $wbsockdir
	nmbd:socket dir = $nmbdsockdir
	idmap config * : range = 100000-200000
	winbind enum users = yes
	winbind enum groups = yes

#	min receivefile size = 4000

	max protocol = SMB2
	read only = no
	server signing = auto

	smbd:sharedelay = 100000
#	smbd:writetimeupdatedelay = 500000
	map hidden = no
	map system = no
	map readonly = no
	store dos attributes = yes
	create mask = 755
	vfs objects = $vfs_modulesdir_abs/xattr_tdb.so $vfs_modulesdir_abs/streams_depot.so

	printing = vlp
	print command = $bindir_abs/vlp tdbfile=$lockdir/vlp.tdb print %p %s
	lpq command = $bindir_abs/vlp tdbfile=$lockdir/vlp.tdb lpq %p
	lp rm command = $bindir_abs/vlp tdbfile=$lockdir/vlp.tdb lprm %p %j
	lp pause command = $bindir_abs/vlp tdbfile=$lockdir/vlp.tdb lppause %p %j
	lp resume command = $bindir_abs/vlp tdbfile=$lockdir/vlp.tdb lpresume %p %j
	queue pause command = $bindir_abs/vlp tdbfile=$lockdir/vlp.tdb queuepause %p
	queue resume command = $bindir_abs/vlp tdbfile=$lockdir/vlp.tdb queueresume %p
	lpq cache time = 0

	ncalrpc dir = $prefix_abs/ncalrpc
	rpc_server:epmapper = external
	rpc_server:spoolss = external
	rpc_server:lsarpc = external
	rpc_server:samr = external
	rpc_server:netlogon = external
	rpc_server:tcpip = yes

	rpc_daemon:epmd = fork
	rpc_daemon:spoolssd = fork
	rpc_daemon:lsasd = fork

        resolv:host file = $dns_host_file

        # The samba3.blackbox.smbclient_s3 test uses this to test that
        # sending messages works, and that the %m sub works.
        message command = mv %s $shrdir/message.%m

	# Begin extra options
	$extra_options
	# End extra options

	#Include user defined custom parameters if set
";

	if (defined($ENV{INCLUDE_CUSTOM_CONF})) {
		print CONF "\t$ENV{INCLUDE_CUSTOM_CONF}\n";
	}

	print CONF "
[tmp]
	path = $shrdir
        comment = smb username is [%U]
[tmpguest]
	path = $shrdir
        guest ok = yes
[guestonly]
	path = $shrdir
        guest only = yes
        guest ok = yes
[forceuser]
	path = $shrdir
        force user = $unix_name
        guest ok = yes
[forcegroup]
	path = $shrdir
        force group = nogroup
        guest ok = yes
[ro-tmp]
	path = $ro_shrdir
	guest ok = yes
[msdfs-share]
	path = $msdfs_shrdir
	msdfs root = yes
	guest ok = yes
[hideunread]
	copy = tmp
	hide unreadable = yes
[tmpcase]
	copy = tmp
	case sensitive = yes
[hideunwrite]
	copy = tmp
	hide unwriteable files = yes
[print1]
	copy = tmp
	printable = yes

[print2]
	copy = print1
[print3]
	copy = print1
[lp]
	copy = print1
[print\$]
	copy = tmp
	";
	close(CONF);

	##
	## create a test account
	##

	unless (open(PASSWD, ">$nss_wrapper_passwd")) {
           warn("Unable to open $nss_wrapper_passwd");
           return undef;
        } 
	print PASSWD "nobody:x:$uid_nobody:$gid_nobody:nobody gecos:$prefix_abs:/bin/false
$unix_name:x:$unix_uid:$unix_gids[0]:$unix_name gecos:$prefix_abs:/bin/false
";
	if ($unix_uid != 0) {
		print PASSWD "root:x:$uid_root:$gid_root:root gecos:$prefix_abs:/bin/false";
	}
	close(PASSWD);

	unless (open(GROUP, ">$nss_wrapper_group")) {
             warn("Unable to open $nss_wrapper_group");
             return undef;
        }
	print GROUP "nobody:x:$gid_nobody:
nogroup:x:$gid_nogroup:nobody
$unix_name-group:x:$unix_gids[0]:
domusers:X:$gid_domusers:
";
	if ($unix_gids[0] != 0) {
		print GROUP "root:x:$gid_root:";
	}

	close(GROUP);

	foreach my $evlog (@eventlog_list) {
		my $evlogtdb = "$eventlogdir/$evlog.tdb";
		open(EVENTLOG, ">$evlogtdb") or die("Unable to open $evlogtdb");
		close(EVENTLOG);
	}

	$ENV{NSS_WRAPPER_PASSWD} = $nss_wrapper_passwd;
	$ENV{NSS_WRAPPER_GROUP} = $nss_wrapper_group;

        my $cmd = Samba::bindir_path($self, "smbpasswd")." -c $conffile -L -s -a $unix_name > /dev/null";
	unless (open(PWD, "|$cmd")) {
             warn("Unable to set password for test account\n$cmd");
             return undef;
        }
	print PWD "$password\n$password\n";
	unless (close(PWD)) {
             warn("Unable to set password for test account\n$cmd");
             return undef; 
        }
	print "DONE\n";

	open(HOSTS, ">>$ENV{SELFTEST_PREFIX}/dns_host_file") or die("Unable to open $ENV{SELFTEST_PREFIX}/dns_host_file");
	print HOSTS "A $server. $server_ip
";
	close(HOSTS);

	$ret{SERVER_IP} = $server_ip;
	$ret{NMBD_TEST_LOG} = "$prefix/nmbd_test.log";
	$ret{NMBD_TEST_LOG_POS} = 0;
	$ret{WINBINDD_TEST_LOG} = "$prefix/winbindd_test.log";
	$ret{WINBINDD_TEST_LOG_POS} = 0;
	$ret{SMBD_TEST_LOG} = "$prefix/smbd_test.log";
	$ret{SMBD_TEST_LOG_POS} = 0;
	$ret{SERVERCONFFILE} = $conffile;
	$ret{CONFIGURATION} ="-s $conffile";
	$ret{SERVER} = $server;
	$ret{USERNAME} = $unix_name;
	$ret{USERID} = $unix_uid;
	$ret{DOMAIN} = $domain;
	$ret{NETBIOSNAME} = $server;
	$ret{PASSWORD} = $password;
	$ret{PIDDIR} = $piddir;
	$ret{WINBINDD_SOCKET_DIR} = $wbsockdir;
	$ret{WINBINDD_PRIV_PIPE_DIR} = $wbsockprivdir;
	$ret{NMBD_SOCKET_DIR} = $nmbdsockdir;
	$ret{SOCKET_WRAPPER_DEFAULT_IFACE} = $swiface;
	$ret{NSS_WRAPPER_PASSWD} = $nss_wrapper_passwd;
	$ret{NSS_WRAPPER_GROUP} = $nss_wrapper_group;
	$ret{NSS_WRAPPER_WINBIND_SO_PATH} = $ENV{NSS_WRAPPER_WINBIND_SO_PATH};
        if (not defined($ret{NSS_WRAPPER_WINBIND_SO_PATH})) {
	        $ret{NSS_WRAPPER_WINBIND_SO_PATH} = Samba::bindir_path($self, "default/nsswitch/libnss-winbind.so");
        }
	$ret{LOCAL_PATH} = "$shrdir";

	return \%ret;
}

sub wait_for_start($$)
{
	my ($self, $envvars) = @_;

	# give time for nbt server to register its names
	print "delaying for nbt name registration\n";
	sleep(10);
	# This will return quickly when things are up, but be slow if we need to wait for (eg) SSL init 
	system(Samba::bindir_path($self, "nmblookup3") ." $envvars->{CONFIGURATION} -U $envvars->{SERVER_IP} __SAMBA__");
	system(Samba::bindir_path($self, "nmblookup3") ." $envvars->{CONFIGURATION} __SAMBA__");
	system(Samba::bindir_path($self, "nmblookup3") ." $envvars->{CONFIGURATION} -U 127.255.255.255 __SAMBA__");
	system(Samba::bindir_path($self, "nmblookup3") ." $envvars->{CONFIGURATION} -U $envvars->{SERVER_IP} $envvars->{SERVER}");
	system(Samba::bindir_path($self, "nmblookup3") ." $envvars->{CONFIGURATION} $envvars->{SERVER}");

	# make sure smbd is also up set
	print "wait for smbd\n";

	my $count = 0;
	my $ret;
	do {
	    $ret = system(Samba::bindir_path($self, "smbclient3") ." $envvars->{CONFIGURATION} -L $envvars->{SERVER} -U% -p 139");
	    if ($ret != 0) {
		sleep(2);
	    }
	    $count++
	} while ($ret != 0 && $count < 10);
	if ($count == 10) {
	    print "SMBD failed to start up in a reasonable time (20sec)\n";
	    teardown_env($self, $envvars);
	    return 0;
	}
	# Ensure we have domain users mapped.
	$ret = system(Samba::bindir_path($self, "net") ." $envvars->{CONFIGURATION} groupmap add rid=513 unixgroup=domusers type=domain");
	if ($ret != 0) {
	    return 1;
	}

	print $self->getlog_env($envvars);

	return 1;
}

1;
