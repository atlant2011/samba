#!/usr/bin/env python
#
# domain management
#
# Copyright Matthias Dieter Wallnoefer 2009
# Copyright Andrew Kroeger 2009
# Copyright Jelmer Vernooij 2009
# Copyright Giampaolo Lauria 2011
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#



import samba.getopt as options
import ldb
import sys, os
import tempfile
import logging
from samba import Ldb
from samba.net import Net, LIBNET_JOIN_AUTOMATIC
from samba.dcerpc.misc import SEC_CHAN_WKSTA
from samba.join import join_RODC, join_DC, join_subdomain
from samba.auth import system_session
from samba.samdb import SamDB
from samba.dcerpc.samr import DOMAIN_PASSWORD_COMPLEX, DOMAIN_PASSWORD_STORE_CLEARTEXT
from samba.netcmd import (
    Command,
    CommandError,
    SuperCommand,
    Option
    )
from samba.samba3 import Samba3
from samba.samba3 import param as s3param
from samba.upgrade import upgrade_from_samba3
from samba.provision import ProvisioningError

from samba.dsdb import (
    DS_DOMAIN_FUNCTION_2000,
    DS_DOMAIN_FUNCTION_2003,
    DS_DOMAIN_FUNCTION_2003_MIXED,
    DS_DOMAIN_FUNCTION_2008,
    DS_DOMAIN_FUNCTION_2008_R2,
    )

def get_testparm_var(testparm, smbconf, varname):
    cmd = "%s -s -l --parameter-name='%s' %s 2>/dev/null" % (testparm, varname, smbconf)
    output = os.popen(cmd, 'r').readline()
    return output.strip()

class cmd_domain_export_keytab(Command):
    """Dumps kerberos keys of the domain into a keytab"""

    synopsis = "%prog domain exportkeytab <keytab> [options]"

    takes_options = [
        ]

    takes_args = ["keytab"]

    def run(self, keytab, credopts=None, sambaopts=None, versionopts=None):
        lp = sambaopts.get_loadparm()
        net = Net(None, lp, server=credopts.ipaddress)
        net.export_keytab(keytab=keytab)



class cmd_domain_join(Command):
    """Joins domain as either member or backup domain controller *"""

    synopsis = "%prog domain join <dnsdomain> [DC|RODC|MEMBER|SUBDOMAIN] [options]"

    takes_options = [
        Option("--server", help="DC to join", type=str),
        Option("--site", help="site to join", type=str),
        Option("--targetdir", help="where to store provision", type=str),
        Option("--parent-domain", help="parent domain to create subdomain under", type=str),
        Option("--domain-critical-only",
               help="only replicate critical domain objects",
               action="store_true"),
        ]

    takes_args = ["domain", "role?"]

    def run(self, domain, role=None, sambaopts=None, credopts=None,
            versionopts=None, server=None, site=None, targetdir=None,
            domain_critical_only=False, parent_domain=None):
        lp = sambaopts.get_loadparm()
        creds = credopts.get_credentials(lp)
        net = Net(creds, lp, server=credopts.ipaddress)

        if site is None:
            site = "Default-First-Site-Name"

        netbios_name = lp.get("netbios name")

        if not role is None:
            role = role.upper()

        if role is None or role == "MEMBER":
            (join_password, sid, domain_name) = net.join_member(domain,
                                                                netbios_name,
                                                                LIBNET_JOIN_AUTOMATIC)

            self.outf.write("Joined domain %s (%s)\n" % (domain_name, sid))
            return
        elif role == "DC":
            join_DC(server=server, creds=creds, lp=lp, domain=domain,
                    site=site, netbios_name=netbios_name, targetdir=targetdir,
                    domain_critical_only=domain_critical_only)
            return
        elif role == "RODC":
            join_RODC(server=server, creds=creds, lp=lp, domain=domain,
                      site=site, netbios_name=netbios_name, targetdir=targetdir,
                      domain_critical_only=domain_critical_only)
            return
        elif role == "SUBDOMAIN":
            netbios_domain = lp.get("workgroup")
            if parent_domain is None:
                parent_domain = ".".join(domain.split(".")[1:])
            join_subdomain(server=server, creds=creds, lp=lp, dnsdomain=domain, parent_domain=parent_domain,
                           site=site, netbios_name=netbios_name, netbios_domain=netbios_domain, targetdir=targetdir)
            return
        else:
            raise CommandError("Invalid role %s (possible values: MEMBER, DC, RODC)" % role)



class cmd_domain_level(Command):
    """Raises domain and forest function levels"""

    synopsis = "%prog domain level (show|raise <options>) [options]"

    takes_options = [
        Option("-H", "--URL", help="LDB URL for database or target server", type=str,
               metavar="URL", dest="H"),
        Option("--quiet", help="Be quiet", action="store_true"),
        Option("--forest-level", type="choice", choices=["2003", "2008", "2008_R2"],
            help="The forest function level (2003 | 2008 | 2008_R2)"),
        Option("--domain-level", type="choice", choices=["2003", "2008", "2008_R2"],
            help="The domain function level (2003 | 2008 | 2008_R2)")
            ]

    takes_args = ["subcommand"]

    def run(self, subcommand, H=None, forest_level=None, domain_level=None,
            quiet=False, credopts=None, sambaopts=None, versionopts=None):
        lp = sambaopts.get_loadparm()
        creds = credopts.get_credentials(lp, fallback_machine=True)

        samdb = SamDB(url=H, session_info=system_session(),
            credentials=creds, lp=lp)

        domain_dn = samdb.domain_dn()

        res_forest = samdb.search("CN=Partitions,%s" % samdb.get_config_basedn(),
          scope=ldb.SCOPE_BASE, attrs=["msDS-Behavior-Version"])
        assert len(res_forest) == 1

        res_domain = samdb.search(domain_dn, scope=ldb.SCOPE_BASE,
          attrs=["msDS-Behavior-Version", "nTMixedDomain"])
        assert len(res_domain) == 1

        res_dc_s = samdb.search("CN=Sites,%s" % samdb.get_config_basedn(),
          scope=ldb.SCOPE_SUBTREE, expression="(objectClass=nTDSDSA)",
          attrs=["msDS-Behavior-Version"])
        assert len(res_dc_s) >= 1

        try:
            level_forest = int(res_forest[0]["msDS-Behavior-Version"][0])
            level_domain = int(res_domain[0]["msDS-Behavior-Version"][0])
            level_domain_mixed = int(res_domain[0]["nTMixedDomain"][0])

            min_level_dc = int(res_dc_s[0]["msDS-Behavior-Version"][0]) # Init value
            for msg in res_dc_s:
                if int(msg["msDS-Behavior-Version"][0]) < min_level_dc:
                    min_level_dc = int(msg["msDS-Behavior-Version"][0])

            if level_forest < 0 or level_domain < 0:
                raise CommandError("Domain and/or forest function level(s) is/are invalid. Correct them or reprovision!")
            if min_level_dc < 0:
                raise CommandError("Lowest function level of a DC is invalid. Correct this or reprovision!")
            if level_forest > level_domain:
                raise CommandError("Forest function level is higher than the domain level(s). Correct this or reprovision!")
            if level_domain > min_level_dc:
                raise CommandError("Domain function level is higher than the lowest function level of a DC. Correct this or reprovision!")

        except KeyError:
            raise CommandError("Could not retrieve the actual domain, forest level and/or lowest DC function level!")

        if subcommand == "show":
            self.message("Domain and forest function level for domain '%s'" % domain_dn)
            if level_forest == DS_DOMAIN_FUNCTION_2000 and level_domain_mixed != 0:
                self.message("\nATTENTION: You run SAMBA 4 on a forest function level lower than Windows 2000 (Native). This isn't supported! Please raise!")
            if level_domain == DS_DOMAIN_FUNCTION_2000 and level_domain_mixed != 0:
                self.message("\nATTENTION: You run SAMBA 4 on a domain function level lower than Windows 2000 (Native). This isn't supported! Please raise!")
            if min_level_dc == DS_DOMAIN_FUNCTION_2000 and level_domain_mixed != 0:
                self.message("\nATTENTION: You run SAMBA 4 on a lowest function level of a DC lower than Windows 2003. This isn't supported! Please step-up or upgrade the concerning DC(s)!")

            self.message("")

            if level_forest == DS_DOMAIN_FUNCTION_2000:
                outstr = "2000"
            elif level_forest == DS_DOMAIN_FUNCTION_2003_MIXED:
                outstr = "2003 with mixed domains/interim (NT4 DC support)"
            elif level_forest == DS_DOMAIN_FUNCTION_2003:
                outstr = "2003"
            elif level_forest == DS_DOMAIN_FUNCTION_2008:
                outstr = "2008"
            elif level_forest == DS_DOMAIN_FUNCTION_2008_R2:
                outstr = "2008 R2"
            else:
                outstr = "higher than 2008 R2"
            self.message("Forest function level: (Windows) " + outstr)

            if level_domain == DS_DOMAIN_FUNCTION_2000 and level_domain_mixed != 0:
                outstr = "2000 mixed (NT4 DC support)"
            elif level_domain == DS_DOMAIN_FUNCTION_2000 and level_domain_mixed == 0:
                outstr = "2000"
            elif level_domain == DS_DOMAIN_FUNCTION_2003_MIXED:
                outstr = "2003 with mixed domains/interim (NT4 DC support)"
            elif level_domain == DS_DOMAIN_FUNCTION_2003:
                outstr = "2003"
            elif level_domain == DS_DOMAIN_FUNCTION_2008:
                outstr = "2008"
            elif level_domain == DS_DOMAIN_FUNCTION_2008_R2:
                outstr = "2008 R2"
            else:
                outstr = "higher than 2008 R2"
            self.message("Domain function level: (Windows) " + outstr)

            if min_level_dc == DS_DOMAIN_FUNCTION_2000:
                outstr = "2000"
            elif min_level_dc == DS_DOMAIN_FUNCTION_2003:
                outstr = "2003"
            elif min_level_dc == DS_DOMAIN_FUNCTION_2008:
                outstr = "2008"
            elif min_level_dc == DS_DOMAIN_FUNCTION_2008_R2:
                outstr = "2008 R2"
            else:
                outstr = "higher than 2008 R2"
            self.message("Lowest function level of a DC: (Windows) " + outstr)

        elif subcommand == "raise":
            msgs = []

            if domain_level is not None:
                if domain_level == "2003":
                    new_level_domain = DS_DOMAIN_FUNCTION_2003
                elif domain_level == "2008":
                    new_level_domain = DS_DOMAIN_FUNCTION_2008
                elif domain_level == "2008_R2":
                    new_level_domain = DS_DOMAIN_FUNCTION_2008_R2

                if new_level_domain <= level_domain and level_domain_mixed == 0:
                    raise CommandError("Domain function level can't be smaller equal to the actual one!")

                if new_level_domain > min_level_dc:
                    raise CommandError("Domain function level can't be higher than the lowest function level of a DC!")

                # Deactivate mixed/interim domain support
                if level_domain_mixed != 0:
                    # Directly on the base DN
                    m = ldb.Message()
                    m.dn = ldb.Dn(samdb, domain_dn)
                    m["nTMixedDomain"] = ldb.MessageElement("0",
                      ldb.FLAG_MOD_REPLACE, "nTMixedDomain")
                    samdb.modify(m)
                    # Under partitions
                    m = ldb.Message()
                    m.dn = ldb.Dn(samdb, "CN=" + lp.get("workgroup") + ",CN=Partitions,%s" % ldb.get_config_basedn())
                    m["nTMixedDomain"] = ldb.MessageElement("0",
                      ldb.FLAG_MOD_REPLACE, "nTMixedDomain")
                    try:
                        samdb.modify(m)
                    except ldb.LdbError, (enum, emsg):
                        if enum != ldb.ERR_UNWILLING_TO_PERFORM:
                            raise

                # Directly on the base DN
                m = ldb.Message()
                m.dn = ldb.Dn(samdb, domain_dn)
                m["msDS-Behavior-Version"]= ldb.MessageElement(
                  str(new_level_domain), ldb.FLAG_MOD_REPLACE,
                            "msDS-Behavior-Version")
                samdb.modify(m)
                # Under partitions
                m = ldb.Message()
                m.dn = ldb.Dn(samdb, "CN=" + lp.get("workgroup")
                  + ",CN=Partitions,%s" % ldb.get_config_basedn())
                m["msDS-Behavior-Version"]= ldb.MessageElement(
                  str(new_level_domain), ldb.FLAG_MOD_REPLACE,
                          "msDS-Behavior-Version")
                try:
                    samdb.modify(m)
                except ldb.LdbError, (enum, emsg):
                    if enum != ldb.ERR_UNWILLING_TO_PERFORM:
                        raise

                level_domain = new_level_domain
                msgs.append("Domain function level changed!")

            if forest_level is not None:
                if forest_level == "2003":
                    new_level_forest = DS_DOMAIN_FUNCTION_2003
                elif forest_level == "2008":
                    new_level_forest = DS_DOMAIN_FUNCTION_2008
                elif forest_level == "2008_R2":
                    new_level_forest = DS_DOMAIN_FUNCTION_2008_R2
                if new_level_forest <= level_forest:
                    raise CommandError("Forest function level can't be smaller equal to the actual one!")
                if new_level_forest > level_domain:
                    raise CommandError("Forest function level can't be higher than the domain function level(s). Please raise it/them first!")
                m = ldb.Message()
                m.dn = ldb.Dn(samdb, "CN=Partitions,%s" % ldb.get_config_basedn())
                m["msDS-Behavior-Version"]= ldb.MessageElement(
                  str(new_level_forest), ldb.FLAG_MOD_REPLACE,
                          "msDS-Behavior-Version")
                samdb.modify(m)
                msgs.append("Forest function level changed!")
            msgs.append("All changes applied successfully!")
            self.message("\n".join(msgs))
        else:
            raise CommandError("Wrong argument '%s'!" % subcommand)



class cmd_domain_machinepassword(Command):
    """Gets a machine password out of our SAM"""

    synopsis = "%prog domain machinepassword <accountname> [options]"

    takes_args = ["secret"]

    def run(self, secret, sambaopts=None, credopts=None, versionopts=None):
        lp = sambaopts.get_loadparm()
        creds = credopts.get_credentials(lp, fallback_machine=True)
        name = lp.get("secrets database")
        path = lp.get("private dir")
        url = os.path.join(path, name)
        if not os.path.exists(url):
            raise CommandError("secret database not found at %s " % url)
        secretsdb = Ldb(url=url, session_info=system_session(),
            credentials=creds, lp=lp)
        result = secretsdb.search(attrs=["secret"],
            expression="(&(objectclass=primaryDomain)(samaccountname=%s))" % ldb.binary_encode(secret))

        if len(result) != 1:
            raise CommandError("search returned %d records, expected 1" % len(result))

        self.outf.write("%s\n" % result[0]["secret"])



class cmd_domain_passwordsettings(Command):
    """Sets password settings

    Password complexity, history length, minimum password length, the minimum
    and maximum password age) on a Samba4 server.
    """

    synopsis = "%prog domain passwordsettings (show|set <options>) [options]"

    takes_options = [
        Option("-H", "--URL", help="LDB URL for database or target server", type=str,
               metavar="URL", dest="H"),
        Option("--quiet", help="Be quiet", action="store_true"),
        Option("--complexity", type="choice", choices=["on","off","default"],
          help="The password complexity (on | off | default). Default is 'on'"),
        Option("--store-plaintext", type="choice", choices=["on","off","default"],
          help="Store plaintext passwords where account have 'store passwords with reversible encryption' set (on | off | default). Default is 'off'"),
        Option("--history-length",
          help="The password history length (<integer> | default).  Default is 24.", type=str),
        Option("--min-pwd-length",
          help="The minimum password length (<integer> | default).  Default is 7.", type=str),
        Option("--min-pwd-age",
          help="The minimum password age (<integer in days> | default).  Default is 1.", type=str),
        Option("--max-pwd-age",
          help="The maximum password age (<integer in days> | default).  Default is 43.", type=str),
          ]

    takes_args = ["subcommand"]

    def run(self, subcommand, H=None, min_pwd_age=None, max_pwd_age=None,
            quiet=False, complexity=None, store_plaintext=None, history_length=None,
            min_pwd_length=None, credopts=None, sambaopts=None,
            versionopts=None):
        lp = sambaopts.get_loadparm()
        creds = credopts.get_credentials(lp)

        samdb = SamDB(url=H, session_info=system_session(),
            credentials=creds, lp=lp)

        domain_dn = samdb.domain_dn()
        res = samdb.search(domain_dn, scope=ldb.SCOPE_BASE,
          attrs=["pwdProperties", "pwdHistoryLength", "minPwdLength",
                 "minPwdAge", "maxPwdAge"])
        assert(len(res) == 1)
        try:
            pwd_props = int(res[0]["pwdProperties"][0])
            pwd_hist_len = int(res[0]["pwdHistoryLength"][0])
            cur_min_pwd_len = int(res[0]["minPwdLength"][0])
            # ticks -> days
            cur_min_pwd_age = int(abs(int(res[0]["minPwdAge"][0])) / (1e7 * 60 * 60 * 24))
            cur_max_pwd_age = int(abs(int(res[0]["maxPwdAge"][0])) / (1e7 * 60 * 60 * 24))
        except Exception, e:
            raise CommandError("Could not retrieve password properties!", e)

        if subcommand == "show":
            self.message("Password informations for domain '%s'" % domain_dn)
            self.message("")
            if pwd_props & DOMAIN_PASSWORD_COMPLEX != 0:
                self.message("Password complexity: on")
            else:
                self.message("Password complexity: off")
            if pwd_props & DOMAIN_PASSWORD_STORE_CLEARTEXT != 0:
                self.message("Store plaintext passwords: on")
            else:
                self.message("Store plaintext passwords: off")
            self.message("Password history length: %d" % pwd_hist_len)
            self.message("Minimum password length: %d" % cur_min_pwd_len)
            self.message("Minimum password age (days): %d" % cur_min_pwd_age)
            self.message("Maximum password age (days): %d" % cur_max_pwd_age)
        elif subcommand == "set":
            msgs = []
            m = ldb.Message()
            m.dn = ldb.Dn(samdb, domain_dn)

            if complexity is not None:
                if complexity == "on" or complexity == "default":
                    pwd_props = pwd_props | DOMAIN_PASSWORD_COMPLEX
                    msgs.append("Password complexity activated!")
                elif complexity == "off":
                    pwd_props = pwd_props & (~DOMAIN_PASSWORD_COMPLEX)
                    msgs.append("Password complexity deactivated!")

            if store_plaintext is not None:
                if store_plaintext == "on" or store_plaintext == "default":
                    pwd_props = pwd_props | DOMAIN_PASSWORD_STORE_CLEARTEXT
                    msgs.append("Plaintext password storage for changed passwords activated!")
                elif store_plaintext == "off":
                    pwd_props = pwd_props & (~DOMAIN_PASSWORD_STORE_CLEARTEXT)
                    msgs.append("Plaintext password storage for changed passwords deactivated!")

            if complexity is not None or store_plaintext is not None:
                m["pwdProperties"] = ldb.MessageElement(str(pwd_props),
                  ldb.FLAG_MOD_REPLACE, "pwdProperties")

            if history_length is not None:
                if history_length == "default":
                    pwd_hist_len = 24
                else:
                    pwd_hist_len = int(history_length)

                if pwd_hist_len < 0 or pwd_hist_len > 24:
                    raise CommandError("Password history length must be in the range of 0 to 24!")

                m["pwdHistoryLength"] = ldb.MessageElement(str(pwd_hist_len),
                  ldb.FLAG_MOD_REPLACE, "pwdHistoryLength")
                msgs.append("Password history length changed!")

            if min_pwd_length is not None:
                if min_pwd_length == "default":
                    min_pwd_len = 7
                else:
                    min_pwd_len = int(min_pwd_length)

                if min_pwd_len < 0 or min_pwd_len > 14:
                    raise CommandError("Minimum password length must be in the range of 0 to 14!")

                m["minPwdLength"] = ldb.MessageElement(str(min_pwd_len),
                  ldb.FLAG_MOD_REPLACE, "minPwdLength")
                msgs.append("Minimum password length changed!")

            if min_pwd_age is not None:
                if min_pwd_age == "default":
                    min_pwd_age = 1
                else:
                    min_pwd_age = int(min_pwd_age)

                if min_pwd_age < 0 or min_pwd_age > 998:
                    raise CommandError("Minimum password age must be in the range of 0 to 998!")

                # days -> ticks
                min_pwd_age_ticks = -int(min_pwd_age * (24 * 60 * 60 * 1e7))

                m["minPwdAge"] = ldb.MessageElement(str(min_pwd_age_ticks),
                  ldb.FLAG_MOD_REPLACE, "minPwdAge")
                msgs.append("Minimum password age changed!")

            if max_pwd_age is not None:
                if max_pwd_age == "default":
                    max_pwd_age = 43
                else:
                    max_pwd_age = int(max_pwd_age)

                if max_pwd_age < 0 or max_pwd_age > 999:
                    raise CommandError("Maximum password age must be in the range of 0 to 999!")

                # days -> ticks
                max_pwd_age_ticks = -int(max_pwd_age * (24 * 60 * 60 * 1e7))

                m["maxPwdAge"] = ldb.MessageElement(str(max_pwd_age_ticks),
                  ldb.FLAG_MOD_REPLACE, "maxPwdAge")
                msgs.append("Maximum password age changed!")

            if max_pwd_age > 0 and min_pwd_age >= max_pwd_age:
                raise CommandError("Maximum password age (%d) must be greater than minimum password age (%d)!" % (max_pwd_age, min_pwd_age))

            if len(m) == 0:
                raise CommandError("You must specify at least one option to set. Try --help")
            samdb.modify(m)
            msgs.append("All changes applied successfully!")
            self.message("\n".join(msgs))
        else:
            raise CommandError("Wrong argument '%s'!" % subcommand)


class cmd_domain_samba3upgrade(Command):
    """Upgrade from Samba3 database to Samba4 AD database"""

    synopsis = "%prog domain samba3upgrade [options] <samba3_smb_conf>"

    long_description = """Specify either samba3 database directory (with --libdir) or
samba3 testparm utility (with --testparm)."""

    takes_optiongroups = {
        "sambaopts": options.SambaOptions,
        "versionopts": options.VersionOptions
    }

    takes_options = [
        Option("--libdir", type="string", metavar="DIR",
                  help="Path to samba3 database directory"),
        Option("--testparm", type="string", metavar="PATH",
                  help="Path to samba3 testparm utility"),
        Option("--targetdir", type="string", metavar="DIR",
                  help="Path prefix where the new Samba 4.0 AD domain should be initialised"),
        Option("--quiet", help="Be quiet"),
        Option("--use-xattrs", type="choice", choices=["yes","no","auto"], metavar="[yes|no|auto]",
                   help="Define if we should use the native fs capabilities or a tdb file for storing attributes likes ntacl, auto tries to make an inteligent guess based on the user rights and system capabilities", default="auto"),
    ]

    takes_args = ["smbconf"]

    def run(self, smbconf=None, targetdir=None, libdir=None, testparm=None, 
            quiet=None, use_xattrs=None, sambaopts=None, versionopts=None):

        if not os.path.exists(smbconf):
            raise CommandError("File %s does not exist" % smbconf)
        
        if testparm and not os.path.exists(testparm):
            raise CommandError("Testparm utility %s does not exist" % testparm)

        if libdir and not os.path.exists(libdir):
            raise CommandError("Directory %s does not exist" % libdir)

        if not libdir and not testparm:
            raise CommandError("Please specify either libdir or testparm")

        if libdir and testparm:
            self.outf.write("warning: both libdir and testparm specified, ignoring libdir.\n")
            libdir = None

        logger = logging.getLogger("upgrade")
        logger.addHandler(logging.StreamHandler(sys.stdout))
        if quiet:
            logger.setLevel(logging.WARNING)
        else:
            logger.setLevel(logging.INFO)

        lp = sambaopts.get_loadparm()

        s3conf = s3param.get_context()

        if sambaopts.realm:
            s3conf.set("realm", sambaopts.realm)

        eadb = True
        if use_xattrs == "yes":
            eadb = False
        elif use_xattrs == "auto" and not s3conf.get("posix:eadb"):
            tmpfile = tempfile.NamedTemporaryFile()
            try:
                samba.ntacls.setntacl(lp, tmpfile.name,
                            "O:S-1-5-32G:S-1-5-32", "S-1-5-32", "native")
                eadb = False
            except:
                # FIXME: Don't catch all exceptions here
                logger.info("You are not root or your system do not support xattr, using tdb backend for attributes. "
                            "If you intend to use this provision in production, rerun the script as root on a system supporting xattrs.")
            tmpfile.close()

        # Set correct default values from libdir or testparm
        paths = {}
        if libdir:
            paths["state directory"] = libdir
            paths["private dir"] = libdir
            paths["lock directory"] = libdir
        else:
            paths["state directory"] = get_testparm_var(testparm, smbconf, "state directory")
            paths["private dir"] = get_testparm_var(testparm, smbconf, "private dir")
            paths["lock directory"] = get_testparm_var(testparm, smbconf, "lock directory")
            # "testparm" from Samba 3 < 3.4.x is not aware of the parameter
            # "state directory", instead make use of "lock directory"
            if len(paths["state directory"]) == 0:
                paths["state directory"] = paths["lock directory"]

        for p in paths:
            s3conf.set(p, paths[p])
    
        # load smb.conf parameters
        logger.info("Reading smb.conf")
        s3conf.load(smbconf)
        samba3 = Samba3(smbconf, s3conf)
    
        logger.info("Provisioning")
        upgrade_from_samba3(samba3, logger, targetdir, session_info=system_session(), 
                            useeadb=eadb)


class cmd_domain(SuperCommand):
    """Domain management"""

    subcommands = {}
    subcommands["exportkeytab"] = cmd_domain_export_keytab()
    subcommands["join"] = cmd_domain_join()
    subcommands["level"] = cmd_domain_level()
    subcommands["machinepassword"] = cmd_domain_machinepassword()
    subcommands["passwordsettings"] = cmd_domain_passwordsettings()
    subcommands["samba3upgrade"] = cmd_domain_samba3upgrade()
