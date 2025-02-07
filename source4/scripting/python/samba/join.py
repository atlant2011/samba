#!/usr/bin/env python
#
# python join code
# Copyright Andrew Tridgell 2010
# Copyright Andrew Bartlett 2010
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

"""Joining a domain."""

from samba.auth import system_session
from samba.samdb import SamDB
from samba import gensec, Ldb, drs_utils
import ldb, samba, sys, os, uuid
from samba.ndr import ndr_pack
from samba.dcerpc import security, drsuapi, misc, nbt, lsa, drsblobs
from samba.credentials import Credentials, DONT_USE_KERBEROS
from samba.provision import secretsdb_self_join, provision, provision_fill, FILL_DRS, FILL_SUBDOMAIN
from samba.schema import Schema
from samba.net import Net
from samba.dcerpc import security
import logging
import talloc
import random
import time

# this makes debugging easier
talloc.enable_null_tracking()

class DCJoinException(Exception):

    def __init__(self, msg):
        super(DCJoinException, self).__init__("Can't join, error: %s" % msg)


class dc_join(object):
    '''perform a DC join'''

    def __init__(ctx, server=None, creds=None, lp=None, site=None,
            netbios_name=None, targetdir=None, domain=None):
        ctx.creds = creds
        ctx.lp = lp
        ctx.site = site
        ctx.netbios_name = netbios_name
        ctx.targetdir = targetdir

        ctx.creds.set_gensec_features(creds.get_gensec_features() | gensec.FEATURE_SEAL)
        ctx.net = Net(creds=ctx.creds, lp=ctx.lp)

        if server is not None:
            ctx.server = server
        else:
            print("Finding a writeable DC for domain '%s'" % domain)
            ctx.server = ctx.find_dc(domain)
            print("Found DC %s" % ctx.server)

        ctx.samdb = SamDB(url="ldap://%s" % ctx.server,
                          session_info=system_session(),
                          credentials=ctx.creds, lp=ctx.lp)

        try:
            ctx.samdb.search(scope=ldb.SCOPE_ONELEVEL, attrs=["dn"])
        except ldb.LdbError, (enum, estr):
            raise DCJoinException(estr)


        ctx.myname = netbios_name
        ctx.samname = "%s$" % ctx.myname
        ctx.base_dn = str(ctx.samdb.get_default_basedn())
        ctx.root_dn = str(ctx.samdb.get_root_basedn())
        ctx.schema_dn = str(ctx.samdb.get_schema_basedn())
        ctx.config_dn = str(ctx.samdb.get_config_basedn())
        ctx.domsid = ctx.samdb.get_domain_sid()
        ctx.domain_name = ctx.get_domain_name()
        ctx.invocation_id = misc.GUID(str(uuid.uuid4()))

        ctx.dc_ntds_dn = ctx.get_dsServiceName()
        ctx.dc_dnsHostName = ctx.get_dnsHostName()
        ctx.behavior_version = ctx.get_behavior_version()

        ctx.acct_pass = samba.generate_random_password(32, 40)

        # work out the DNs of all the objects we will be adding
        ctx.server_dn = "CN=%s,CN=Servers,CN=%s,CN=Sites,%s" % (ctx.myname, ctx.site, ctx.config_dn)
        ctx.ntds_dn = "CN=NTDS Settings,%s" % ctx.server_dn
        topology_base = "CN=Topology,CN=Domain System Volume,CN=DFSR-GlobalSettings,CN=System,%s" % ctx.base_dn
        if ctx.dn_exists(topology_base):
            ctx.topology_dn = "CN=%s,%s" % (ctx.myname, topology_base)
        else:
            ctx.topology_dn = None

        ctx.dnsdomain = ctx.samdb.domain_dns_name()
        ctx.dnsforest = ctx.samdb.forest_dns_name()
        ctx.dnshostname = "%s.%s" % (ctx.myname, ctx.dnsdomain)

        ctx.realm = ctx.dnsdomain

        ctx.acct_dn = "CN=%s,OU=Domain Controllers,%s" % (ctx.myname, ctx.base_dn)

        ctx.tmp_samdb = None

        ctx.SPNs = [ "HOST/%s" % ctx.myname,
                     "HOST/%s" % ctx.dnshostname,
                     "GC/%s/%s" % (ctx.dnshostname, ctx.dnsforest) ]

        # these elements are optional
        ctx.never_reveal_sid = None
        ctx.reveal_sid = None
        ctx.connection_dn = None
        ctx.RODC = False
        ctx.krbtgt_dn = None
        ctx.drsuapi = None
        ctx.managedby = None
        ctx.subdomain = False


    def del_noerror(ctx, dn, recursive=False):
        if recursive:
            try:
                res = ctx.samdb.search(base=dn, scope=ldb.SCOPE_ONELEVEL, attrs=["dn"])
            except Exception:
                return
            for r in res:
                ctx.del_noerror(r.dn, recursive=True)
        try:
            ctx.samdb.delete(dn)
            print "Deleted %s" % dn
        except Exception:
            pass

    def cleanup_old_join(ctx):
        '''remove any DNs from a previous join'''
        try:
            # find the krbtgt link
            print("checking samaccountname")
            if ctx.subdomain:
                res = None
            else:
                res = ctx.samdb.search(base=ctx.samdb.get_default_basedn(),
                                       expression='samAccountName=%s' % ldb.binary_encode(ctx.samname),
                                       attrs=["msDS-krbTgtLink"])
                if res:
                    ctx.del_noerror(res[0].dn, recursive=True)
            if ctx.connection_dn is not None:
                ctx.del_noerror(ctx.connection_dn)
            if ctx.krbtgt_dn is not None:
                ctx.del_noerror(ctx.krbtgt_dn)
            ctx.del_noerror(ctx.ntds_dn)
            ctx.del_noerror(ctx.server_dn, recursive=True)
            if ctx.topology_dn:
                ctx.del_noerror(ctx.topology_dn)
            if ctx.partition_dn:
                ctx.del_noerror(ctx.partition_dn)
            if res:
                ctx.new_krbtgt_dn = res[0]["msDS-Krbtgtlink"][0]
                ctx.del_noerror(ctx.new_krbtgt_dn)

            if ctx.subdomain:
                binding_options = "sign"
                lsaconn = lsa.lsarpc("ncacn_ip_tcp:%s[%s]" % (ctx.server, binding_options),
                                     ctx.lp, ctx.creds)

                objectAttr = lsa.ObjectAttribute()
                objectAttr.sec_qos = lsa.QosInfo()

                pol_handle = lsaconn.OpenPolicy2(''.decode('utf-8'),
                                                 objectAttr, security.SEC_FLAG_MAXIMUM_ALLOWED)

                name = lsa.String()
                name.string = ctx.realm
                info = lsaconn.QueryTrustedDomainInfoByName(pol_handle, name, lsa.LSA_TRUSTED_DOMAIN_INFO_FULL_INFO)

                lsaconn.DeleteTrustedDomain(pol_handle, info.info_ex.sid)

                name = lsa.String()
                name.string = ctx.domain_name
                info = lsaconn.QueryTrustedDomainInfoByName(pol_handle, name, lsa.LSA_TRUSTED_DOMAIN_INFO_FULL_INFO)

                lsaconn.DeleteTrustedDomain(pol_handle, info.info_ex.sid)

        except Exception:
            pass

    def find_dc(ctx, domain):
        '''find a writeable DC for the given domain'''
        try:
            ctx.cldap_ret = ctx.net.finddc(domain, nbt.NBT_SERVER_LDAP | nbt.NBT_SERVER_DS | nbt.NBT_SERVER_WRITABLE)
        except Exception:
            raise Exception("Failed to find a writeable DC for domain '%s'" % domain)
        if ctx.cldap_ret.client_site is not None and ctx.cldap_ret.client_site != "":
            ctx.site = ctx.cldap_ret.client_site
        return ctx.cldap_ret.pdc_dns_name


    def get_dsServiceName(ctx):
        res = ctx.samdb.search(base="", scope=ldb.SCOPE_BASE, attrs=["dsServiceName"])
        return res[0]["dsServiceName"][0]

    def get_behavior_version(ctx):
        res = ctx.samdb.search(base=ctx.base_dn, scope=ldb.SCOPE_BASE, attrs=["msDS-Behavior-Version"])
        if "msDS-Behavior-Version" in res[0]:
            return int(res[0]["msDS-Behavior-Version"][0])
        else:
            return samba.dsdb.DS_DOMAIN_FUNCTION_2000

    def get_dnsHostName(ctx):
        res = ctx.samdb.search(base="", scope=ldb.SCOPE_BASE, attrs=["dnsHostName"])
        return res[0]["dnsHostName"][0]

    def get_domain_name(ctx):
        '''get netbios name of the domain from the partitions record'''
        partitions_dn = ctx.samdb.get_partitions_dn()
        res = ctx.samdb.search(base=partitions_dn, scope=ldb.SCOPE_ONELEVEL, attrs=["nETBIOSName"],
                               expression='ncName=%s' % ctx.samdb.get_default_basedn())
        return res[0]["nETBIOSName"][0]

    def get_parent_partition_dn(ctx):
        '''get the parent domain partition DN from parent DNS name'''
        res = ctx.samdb.search(base=ctx.config_dn, attrs=[],
                               expression='(&(objectclass=crossRef)(dnsRoot=%s)(systemFlags:%s:=%u))' %
                               (ctx.parent_dnsdomain, ldb.OID_COMPARATOR_AND, samba.dsdb.SYSTEM_FLAG_CR_NTDS_DOMAIN))
        return str(res[0].dn)

    def get_mysid(ctx):
        '''get the SID of the connected user. Only works with w2k8 and later,
           so only used for RODC join'''
        res = ctx.samdb.search(base="", scope=ldb.SCOPE_BASE, attrs=["tokenGroups"])
        binsid = res[0]["tokenGroups"][0]
        return ctx.samdb.schema_format_value("objectSID", binsid)

    def dn_exists(ctx, dn):
        '''check if a DN exists'''
        try:
            res = ctx.samdb.search(base=dn, scope=ldb.SCOPE_BASE, attrs=[])
        except ldb.LdbError, (enum, estr):
            if enum == ldb.ERR_NO_SUCH_OBJECT:
                return False
            raise
        return True

    def add_krbtgt_account(ctx):
        '''RODCs need a special krbtgt account'''
        print "Adding %s" % ctx.krbtgt_dn
        rec = {
            "dn" : ctx.krbtgt_dn,
            "objectclass" : "user",
            "useraccountcontrol" : str(samba.dsdb.UF_NORMAL_ACCOUNT |
                                       samba.dsdb.UF_ACCOUNTDISABLE),
            "showinadvancedviewonly" : "TRUE",
            "description" : "krbtgt for %s" % ctx.samname}
        ctx.samdb.add(rec, ["rodc_join:1:1"])

        # now we need to search for the samAccountName attribute on the krbtgt DN,
        # as this will have been magically set to the krbtgt number
        res = ctx.samdb.search(base=ctx.krbtgt_dn, scope=ldb.SCOPE_BASE, attrs=["samAccountName"])
        ctx.krbtgt_name = res[0]["samAccountName"][0]

        print "Got krbtgt_name=%s" % ctx.krbtgt_name

        m = ldb.Message()
        m.dn = ldb.Dn(ctx.samdb, ctx.acct_dn)
        m["msDS-krbTgtLink"] = ldb.MessageElement(ctx.krbtgt_dn,
                                                  ldb.FLAG_MOD_REPLACE, "msDS-krbTgtLink")
        ctx.samdb.modify(m)

        ctx.new_krbtgt_dn = "CN=%s,CN=Users,%s" % (ctx.krbtgt_name, ctx.base_dn)
        print "Renaming %s to %s" % (ctx.krbtgt_dn, ctx.new_krbtgt_dn)
        ctx.samdb.rename(ctx.krbtgt_dn, ctx.new_krbtgt_dn)

    def drsuapi_connect(ctx):
        '''make a DRSUAPI connection to the server'''
        binding_options = "seal"
        if int(ctx.lp.get("log level")) >= 5:
            binding_options += ",print"
        binding_string = "ncacn_ip_tcp:%s[%s]" % (ctx.server, binding_options)
        ctx.drsuapi = drsuapi.drsuapi(binding_string, ctx.lp, ctx.creds)
        (ctx.drsuapi_handle, ctx.bind_supported_extensions) = drs_utils.drs_DsBind(ctx.drsuapi)

    def create_tmp_samdb(ctx):
        '''create a temporary samdb object for schema queries'''
        ctx.tmp_schema = Schema(security.dom_sid(ctx.domsid),
                                schemadn=ctx.schema_dn)
        ctx.tmp_samdb = SamDB(session_info=system_session(), url=None, auto_connect=False,
                              credentials=ctx.creds, lp=ctx.lp, global_schema=False,
                              am_rodc=False)
        ctx.tmp_samdb.set_schema(ctx.tmp_schema)

    def build_DsReplicaAttribute(ctx, attrname, attrvalue):
        '''build a DsReplicaAttributeCtr object'''
        r = drsuapi.DsReplicaAttribute()
        r.attid = ctx.tmp_samdb.get_attid_from_lDAPDisplayName(attrname)
        r.value_ctr = 1


    def DsAddEntry(ctx, recs):
        '''add a record via the DRSUAPI DsAddEntry call'''
        if ctx.drsuapi is None:
            ctx.drsuapi_connect()
        if ctx.tmp_samdb is None:
            ctx.create_tmp_samdb()

        objects = []
        for rec in recs:
            id = drsuapi.DsReplicaObjectIdentifier()
            id.dn = rec['dn']

            attrs = []
            for a in rec:
                if a == 'dn':
                    continue
                if not isinstance(rec[a], list):
                    v = [rec[a]]
                else:
                    v = rec[a]
                rattr = ctx.tmp_samdb.dsdb_DsReplicaAttribute(ctx.tmp_samdb, a, v)
                attrs.append(rattr)

            attribute_ctr = drsuapi.DsReplicaAttributeCtr()
            attribute_ctr.num_attributes = len(attrs)
            attribute_ctr.attributes = attrs

            object = drsuapi.DsReplicaObject()
            object.identifier = id
            object.attribute_ctr = attribute_ctr

            list_object = drsuapi.DsReplicaObjectListItem()
            list_object.object = object
            objects.append(list_object)

        req2 = drsuapi.DsAddEntryRequest2()
        req2.first_object = objects[0]
        prev = req2.first_object
        for o in objects[1:]:
            prev.next_object = o
            prev = o

        (level, ctr) = ctx.drsuapi.DsAddEntry(ctx.drsuapi_handle, 2, req2)
        if ctr.err_ver != 1:
            raise RuntimeError("expected err_ver 1, got %u" % ctr.err_ver)
        if ctr.err_data.status != (0, 'WERR_OK'):
            print("DsAddEntry failed with status %s info %s" % (ctr.err_data.status,
                                                                ctr.err_data.info.extended_err))
            raise RuntimeError("DsAddEntry failed")
        if ctr.err_data.dir_err != drsuapi.DRSUAPI_DIRERR_OK:
            print("DsAddEntry failed with dir_err %u" % ctr.err_data.dir_err)
            raise RuntimeError("DsAddEntry failed")
        return ctr.objects


    def join_add_ntdsdsa(ctx):
        '''add the ntdsdsa object'''
        # FIXME: the partition (NC) assignment has to be made dynamic
        print "Adding %s" % ctx.ntds_dn
        rec = {
            "dn" : ctx.ntds_dn,
            "objectclass" : "nTDSDSA",
            "systemFlags" : str(samba.dsdb.SYSTEM_FLAG_DISALLOW_MOVE_ON_DELETE),
            "dMDLocation" : ctx.schema_dn}

        nc_list = [ ctx.base_dn, ctx.config_dn, ctx.schema_dn ]

        if ctx.behavior_version >= samba.dsdb.DS_DOMAIN_FUNCTION_2003:
            rec["msDS-Behavior-Version"] = str(ctx.behavior_version)

        if ctx.behavior_version >= samba.dsdb.DS_DOMAIN_FUNCTION_2003:
            rec["msDS-HasDomainNCs"] = ctx.base_dn

        if ctx.RODC:
            rec["objectCategory"] = "CN=NTDS-DSA-RO,%s" % ctx.schema_dn
            rec["msDS-HasFullReplicaNCs"] = nc_list
            rec["options"] = "37"
            ctx.samdb.add(rec, ["rodc_join:1:1"])
        else:
            rec["objectCategory"] = "CN=NTDS-DSA,%s" % ctx.schema_dn
            rec["HasMasterNCs"]      = nc_list
            if ctx.behavior_version >= samba.dsdb.DS_DOMAIN_FUNCTION_2003:
                rec["msDS-HasMasterNCs"] = nc_list
            rec["options"] = "1"
            rec["invocationId"] = ndr_pack(ctx.invocation_id)
            ctx.DsAddEntry([rec])

        # find the GUID of our NTDS DN
        res = ctx.samdb.search(base=ctx.ntds_dn, scope=ldb.SCOPE_BASE, attrs=["objectGUID"])
        ctx.ntds_guid = misc.GUID(ctx.samdb.schema_format_value("objectGUID", res[0]["objectGUID"][0]))


    def join_add_objects(ctx):
        '''add the various objects needed for the join'''
        if ctx.acct_dn:
            print "Adding %s" % ctx.acct_dn
            rec = {
                "dn" : ctx.acct_dn,
                "objectClass": "computer",
                "displayname": ctx.samname,
                "samaccountname" : ctx.samname,
                "userAccountControl" : str(ctx.userAccountControl | samba.dsdb.UF_ACCOUNTDISABLE),
                "dnshostname" : ctx.dnshostname}
            if ctx.behavior_version >= samba.dsdb.DS_DOMAIN_FUNCTION_2008:
                rec['msDS-SupportedEncryptionTypes'] = str(samba.dsdb.ENC_ALL_TYPES)
            if ctx.managedby:
                rec["managedby"] = ctx.managedby
            if ctx.never_reveal_sid:
                rec["msDS-NeverRevealGroup"] = ctx.never_reveal_sid
            if ctx.reveal_sid:
                rec["msDS-RevealOnDemandGroup"] = ctx.reveal_sid
            ctx.samdb.add(rec)

        if ctx.krbtgt_dn:
            ctx.add_krbtgt_account()

        print "Adding %s" % ctx.server_dn
        rec = {
            "dn": ctx.server_dn,
            "objectclass" : "server",
            # windows uses 50000000 decimal for systemFlags. A windows hex/decimal mixup bug?
            "systemFlags" : str(samba.dsdb.SYSTEM_FLAG_CONFIG_ALLOW_RENAME |
                                samba.dsdb.SYSTEM_FLAG_CONFIG_ALLOW_LIMITED_MOVE |
                                samba.dsdb.SYSTEM_FLAG_DISALLOW_MOVE_ON_DELETE),
            # windows seems to add the dnsHostName later
            "dnsHostName" : ctx.dnshostname}

        if ctx.acct_dn:
            rec["serverReference"] = ctx.acct_dn

        ctx.samdb.add(rec)

        if ctx.subdomain:
            # the rest is done after replication
            ctx.ntds_guid = None
            return

        ctx.join_add_ntdsdsa()

        if ctx.connection_dn is not None:
            print "Adding %s" % ctx.connection_dn
            rec = {
                "dn" : ctx.connection_dn,
                "objectclass" : "nTDSConnection",
                "enabledconnection" : "TRUE",
                "options" : "65",
                "fromServer" : ctx.dc_ntds_dn}
            ctx.samdb.add(rec)

        if ctx.topology_dn and ctx.acct_dn:
            print "Adding %s" % ctx.topology_dn
            rec = {
                "dn" : ctx.topology_dn,
                "objectclass" : "msDFSR-Member",
                "msDFSR-ComputerReference" : ctx.acct_dn,
                "serverReference" : ctx.ntds_dn}
            ctx.samdb.add(rec)

        if ctx.acct_dn:
            print "Adding SPNs to %s" % ctx.acct_dn
            m = ldb.Message()
            m.dn = ldb.Dn(ctx.samdb, ctx.acct_dn)
            for i in range(len(ctx.SPNs)):
                ctx.SPNs[i] = ctx.SPNs[i].replace("$NTDSGUID", str(ctx.ntds_guid))
            m["servicePrincipalName"] = ldb.MessageElement(ctx.SPNs,
                                                           ldb.FLAG_MOD_ADD,
                                                           "servicePrincipalName")
            ctx.samdb.modify(m)

            print "Setting account password for %s" % ctx.samname
            ctx.samdb.setpassword("(&(objectClass=user)(sAMAccountName=%s))" % ldb.binary_encode(ctx.samname),
                                  ctx.acct_pass,
                                  force_change_at_next_login=False,
                                  username=ctx.samname)
            res = ctx.samdb.search(base=ctx.acct_dn, scope=ldb.SCOPE_BASE, attrs=["msDS-keyVersionNumber"])
            ctx.key_version_number = int(res[0]["msDS-keyVersionNumber"][0])

            print("Enabling account")
            m = ldb.Message()
            m.dn = ldb.Dn(ctx.samdb, ctx.acct_dn)
            m["userAccountControl"] = ldb.MessageElement(str(ctx.userAccountControl),
                                                         ldb.FLAG_MOD_REPLACE,
                                                         "userAccountControl")
            ctx.samdb.modify(m)


    def join_add_objects2(ctx):
        '''add the various objects needed for the join, for subdomains post replication'''

        print "Adding %s" % ctx.partition_dn
        # NOTE: windows sends a ntSecurityDescriptor here, we
        # let it default
        rec = {
            "dn" : ctx.partition_dn,
            "objectclass" : "crossRef",
            "objectCategory" : "CN=Cross-Ref,%s" % ctx.schema_dn,
            "nCName" : ctx.base_dn,
            "nETBIOSName" : ctx.domain_name,
            "dnsRoot": ctx.dnsdomain,
            "trustParent" : ctx.parent_partition_dn,
            "systemFlags" : str(samba.dsdb.SYSTEM_FLAG_CR_NTDS_NC|samba.dsdb.SYSTEM_FLAG_CR_NTDS_DOMAIN)}
        if ctx.behavior_version >= samba.dsdb.DS_DOMAIN_FUNCTION_2003:
            rec["msDS-Behavior-Version"] = str(ctx.behavior_version)

        rec2 = {
            "dn" : ctx.ntds_dn,
            "objectclass" : "nTDSDSA",
            "systemFlags" : str(samba.dsdb.SYSTEM_FLAG_DISALLOW_MOVE_ON_DELETE),
            "dMDLocation" : ctx.schema_dn}

        nc_list = [ ctx.base_dn, ctx.config_dn, ctx.schema_dn ]

        if ctx.behavior_version >= samba.dsdb.DS_DOMAIN_FUNCTION_2003:
            rec2["msDS-Behavior-Version"] = str(ctx.behavior_version)

        if ctx.behavior_version >= samba.dsdb.DS_DOMAIN_FUNCTION_2003:
            rec2["msDS-HasDomainNCs"] = ctx.base_dn

        rec2["objectCategory"] = "CN=NTDS-DSA,%s" % ctx.schema_dn
        rec2["HasMasterNCs"]      = nc_list
        if ctx.behavior_version >= samba.dsdb.DS_DOMAIN_FUNCTION_2003:
            rec2["msDS-HasMasterNCs"] = nc_list
        rec2["options"] = "1"
        rec2["invocationId"] = ndr_pack(ctx.invocation_id)

        objects = ctx.DsAddEntry([rec, rec2])
        if len(objects) != 2:
            raise DCJoinException("Expected 2 objects from DsAddEntry")

        ctx.ntds_guid = objects[1].guid

        print("Replicating partition DN")
        ctx.repl.replicate(ctx.partition_dn,
                           misc.GUID("00000000-0000-0000-0000-000000000000"),
                           ctx.ntds_guid,
                           exop=drsuapi.DRSUAPI_EXOP_REPL_OBJ,
                           replica_flags=drsuapi.DRSUAPI_DRS_WRIT_REP)

        print("Replicating NTDS DN")
        ctx.repl.replicate(ctx.ntds_dn,
                           misc.GUID("00000000-0000-0000-0000-000000000000"),
                           ctx.ntds_guid,
                           exop=drsuapi.DRSUAPI_EXOP_REPL_OBJ,
                           replica_flags=drsuapi.DRSUAPI_DRS_WRIT_REP)

    def join_provision(ctx):
        '''provision the local SAM'''

        print "Calling bare provision"

        logger = logging.getLogger("provision")
        logger.addHandler(logging.StreamHandler(sys.stdout))
        smbconf = ctx.lp.configfile

        presult = provision(logger, system_session(), None,
                            smbconf=smbconf, targetdir=ctx.targetdir, samdb_fill=FILL_DRS,
                            realm=ctx.realm, rootdn=ctx.root_dn, domaindn=ctx.base_dn,
                            schemadn=ctx.schema_dn,
                            configdn=ctx.config_dn,
                            serverdn=ctx.server_dn, domain=ctx.domain_name,
                            hostname=ctx.myname, domainsid=ctx.domsid,
                            machinepass=ctx.acct_pass, serverrole="domain controller",
                            sitename=ctx.site, lp=ctx.lp, ntdsguid=ctx.ntds_guid)
        print "Provision OK for domain DN %s" % presult.domaindn
        ctx.local_samdb = presult.samdb
        ctx.lp          = presult.lp
        ctx.paths       = presult.paths
        ctx.names       = presult.names

    def join_provision_own_domain(ctx):
        '''provision the local SAM'''

        # we now operate exclusively on the local database, which
        # we need to reopen in order to get the newly created schema
        print("Reconnecting to local samdb")
        ctx.samdb = SamDB(url=ctx.local_samdb.url,
                          session_info=system_session(),
                          lp=ctx.local_samdb.lp,
                          global_schema=False)
        ctx.samdb.set_invocation_id(str(ctx.invocation_id))
        ctx.local_samdb = ctx.samdb

        print("Finding domain GUID from ncName")
        res = ctx.local_samdb.search(base=ctx.partition_dn, scope=ldb.SCOPE_BASE, attrs=['ncName'],
                                     controls=["extended_dn:1:1"])
        domguid = str(misc.GUID(ldb.Dn(ctx.samdb, res[0]['ncName'][0]).get_extended_component('GUID')))
        print("Got domain GUID %s" % domguid)

        print("Calling own domain provision")

        logger = logging.getLogger("provision")
        logger.addHandler(logging.StreamHandler(sys.stdout))

        secrets_ldb = Ldb(ctx.paths.secrets, session_info=system_session(), lp=ctx.lp)

        presult = provision_fill(ctx.local_samdb, secrets_ldb,
                                 logger, ctx.names, ctx.paths, domainsid=security.dom_sid(ctx.domsid),
                                 domainguid=domguid,
                                 targetdir=ctx.targetdir, samdb_fill=FILL_SUBDOMAIN,
                                 machinepass=ctx.acct_pass, serverrole="domain controller",
                                 lp=ctx.lp, hostip=ctx.names.hostip, hostip6=ctx.names.hostip6)
        print("Provision OK for domain %s" % ctx.names.dnsdomain)


    def join_replicate(ctx):
        '''replicate the SAM'''

        print "Starting replication"
        ctx.local_samdb.transaction_start()
        try:
            source_dsa_invocation_id = misc.GUID(ctx.samdb.get_invocation_id())
            if ctx.ntds_guid is None:
                print("Using DS_BIND_GUID_W2K3")
                destination_dsa_guid = misc.GUID(drsuapi.DRSUAPI_DS_BIND_GUID_W2K3)
            else:
                destination_dsa_guid = ctx.ntds_guid

            if ctx.RODC:
                repl_creds = Credentials()
                repl_creds.guess(ctx.lp)
                repl_creds.set_kerberos_state(DONT_USE_KERBEROS)
                repl_creds.set_username(ctx.samname)
                repl_creds.set_password(ctx.acct_pass)
            else:
                repl_creds = ctx.creds

            binding_options = "seal"
            if int(ctx.lp.get("log level")) >= 5:
                binding_options += ",print"
            repl = drs_utils.drs_Replicate(
                "ncacn_ip_tcp:%s[%s]" % (ctx.server, binding_options),
                ctx.lp, repl_creds, ctx.local_samdb)

            repl.replicate(ctx.schema_dn, source_dsa_invocation_id,
                    destination_dsa_guid, schema=True, rodc=ctx.RODC,
                    replica_flags=ctx.replica_flags)
            repl.replicate(ctx.config_dn, source_dsa_invocation_id,
                    destination_dsa_guid, rodc=ctx.RODC,
                    replica_flags=ctx.replica_flags)
            if not ctx.subdomain:
                repl.replicate(ctx.base_dn, source_dsa_invocation_id,
                               destination_dsa_guid, rodc=ctx.RODC,
                               replica_flags=ctx.domain_replica_flags)
            if ctx.RODC:
                repl.replicate(ctx.acct_dn, source_dsa_invocation_id,
                        destination_dsa_guid,
                        exop=drsuapi.DRSUAPI_EXOP_REPL_SECRET, rodc=True)
                repl.replicate(ctx.new_krbtgt_dn, source_dsa_invocation_id,
                        destination_dsa_guid,
                        exop=drsuapi.DRSUAPI_EXOP_REPL_SECRET, rodc=True)
            ctx.repl = repl
            ctx.source_dsa_invocation_id = source_dsa_invocation_id
            ctx.destination_dsa_guid = destination_dsa_guid

            print "Committing SAM database"
        except:
            ctx.local_samdb.transaction_cancel()
            raise
        else:
            ctx.local_samdb.transaction_commit()


    def join_finalise(ctx):
        '''finalise the join, mark us synchronised and setup secrets db'''

        print "Setting isSynchronized and dsServiceName"
        m = ldb.Message()
        m.dn = ldb.Dn(ctx.local_samdb, '@ROOTDSE')
        m["isSynchronized"] = ldb.MessageElement("TRUE", ldb.FLAG_MOD_REPLACE, "isSynchronized")
        m["dsServiceName"] = ldb.MessageElement("<GUID=%s>" % str(ctx.ntds_guid),
                                                ldb.FLAG_MOD_REPLACE, "dsServiceName")
        ctx.local_samdb.modify(m)

        if ctx.subdomain:
            return

        secrets_ldb = Ldb(ctx.paths.secrets, session_info=system_session(), lp=ctx.lp)

        print "Setting up secrets database"
        secretsdb_self_join(secrets_ldb, domain=ctx.domain_name,
                            realm=ctx.realm,
                            dnsdomain=ctx.dnsdomain,
                            netbiosname=ctx.myname,
                            domainsid=security.dom_sid(ctx.domsid),
                            machinepass=ctx.acct_pass,
                            secure_channel_type=ctx.secure_channel_type,
                            key_version_number=ctx.key_version_number)

    def join_setup_trusts(ctx):
        '''provision the local SAM'''

        def arcfour_encrypt(key, data):
            from Crypto.Cipher import ARC4
            c = ARC4.new(key)
            return c.encrypt(data)

        def string_to_array(string):
            blob = [0] * len(string)

            for i in range(len(string)):
                blob[i] = ord(string[i])

            return blob

        print "Setup domain trusts with server %s" % ctx.server
        binding_options = ""  # why doesn't signing work gere? w2k8r2 claims no session key
        lsaconn = lsa.lsarpc("ncacn_np:%s[%s]" % (ctx.server, binding_options),
                             ctx.lp, ctx.creds)

        objectAttr = lsa.ObjectAttribute()
        objectAttr.sec_qos = lsa.QosInfo()

        pol_handle = lsaconn.OpenPolicy2(''.decode('utf-8'),
                                         objectAttr, security.SEC_FLAG_MAXIMUM_ALLOWED)

        info = lsa.TrustDomainInfoInfoEx()
        info.domain_name.string = ctx.dnsdomain
        info.netbios_name.string = ctx.domain_name
        info.sid = security.dom_sid(ctx.domsid)
        info.trust_direction = lsa.LSA_TRUST_DIRECTION_INBOUND | lsa.LSA_TRUST_DIRECTION_OUTBOUND
        info.trust_type = lsa.LSA_TRUST_TYPE_UPLEVEL
        info.trust_attributes = lsa.LSA_TRUST_ATTRIBUTE_WITHIN_FOREST

        try:
            oldname = lsa.String()
            oldname.string = ctx.dnsdomain
            oldinfo = lsaconn.QueryTrustedDomainInfoByName(pol_handle, oldname,
                                                           lsa.LSA_TRUSTED_DOMAIN_INFO_FULL_INFO)
            print("Removing old trust record for %s (SID %s)" % (ctx.dnsdomain, oldinfo.info_ex.sid))
            lsaconn.DeleteTrustedDomain(pol_handle, oldinfo.info_ex.sid)
        except RuntimeError:
            pass

        password_blob = string_to_array(ctx.trustdom_pass.encode('utf-16-le'))

        clear_value = drsblobs.AuthInfoClear()
        clear_value.size = len(password_blob)
        clear_value.password = password_blob

        clear_authentication_information = drsblobs.AuthenticationInformation()
        clear_authentication_information.LastUpdateTime = samba.unix2nttime(int(time.time()))
        clear_authentication_information.AuthType = lsa.TRUST_AUTH_TYPE_CLEAR
        clear_authentication_information.AuthInfo = clear_value

        authentication_information_array = drsblobs.AuthenticationInformationArray()
        authentication_information_array.count = 1
        authentication_information_array.array = [clear_authentication_information]

        outgoing = drsblobs.trustAuthInOutBlob()
        outgoing.count = 1
        outgoing.current = authentication_information_array

        trustpass = drsblobs.trustDomainPasswords()
        confounder = [3] * 512

        for i in range(512):
            confounder[i] = random.randint(0, 255)

        trustpass.confounder = confounder

        trustpass.outgoing = outgoing
        trustpass.incoming = outgoing

        trustpass_blob = ndr_pack(trustpass)

        encrypted_trustpass = arcfour_encrypt(lsaconn.session_key, trustpass_blob)

        auth_blob = lsa.DATA_BUF2()
        auth_blob.size = len(encrypted_trustpass)
        auth_blob.data = string_to_array(encrypted_trustpass)

        auth_info = lsa.TrustDomainInfoAuthInfoInternal()
        auth_info.auth_blob = auth_blob

        trustdom_handle = lsaconn.CreateTrustedDomainEx2(pol_handle,
                                                         info,
                                                         auth_info,
                                                         security.SEC_STD_DELETE)

        rec = {
            "dn" : "cn=%s,cn=system,%s" % (ctx.parent_dnsdomain, ctx.base_dn),
            "objectclass" : "trustedDomain",
            "trustType" : str(info.trust_type),
            "trustAttributes" : str(info.trust_attributes),
            "trustDirection" : str(info.trust_direction),
            "flatname" : ctx.parent_domain_name,
            "trustPartner" : ctx.parent_dnsdomain,
            "trustAuthIncoming" : ndr_pack(outgoing),
            "trustAuthOutgoing" : ndr_pack(outgoing)
            }
        ctx.local_samdb.add(rec)

        rec = {
            "dn" : "cn=%s$,cn=users,%s" % (ctx.parent_domain_name, ctx.base_dn),
            "objectclass" : "user",
            "userAccountControl" : str(samba.dsdb.UF_INTERDOMAIN_TRUST_ACCOUNT),
            "clearTextPassword" : ctx.trustdom_pass.encode('utf-16-le')
            }
        ctx.local_samdb.add(rec)


    def do_join(ctx):
        ctx.cleanup_old_join()
        try:
            ctx.join_add_objects()
            ctx.join_provision()
            ctx.join_replicate()
            if ctx.subdomain:
                ctx.join_add_objects2()
                ctx.join_provision_own_domain()
                ctx.join_setup_trusts()
            ctx.join_finalise()
        except Exception:
            print "Join failed - cleaning up"
            #ctx.cleanup_old_join()
            raise


def join_RODC(server=None, creds=None, lp=None, site=None, netbios_name=None,
              targetdir=None, domain=None, domain_critical_only=False):
    """join as a RODC"""

    ctx = dc_join(server, creds, lp, site, netbios_name, targetdir, domain)

    lp.set("workgroup", ctx.domain_name)
    print("workgroup is %s" % ctx.domain_name)

    lp.set("realm", ctx.realm)
    print("realm is %s" % ctx.realm)

    ctx.krbtgt_dn = "CN=krbtgt_%s,CN=Users,%s" % (ctx.myname, ctx.base_dn)

    # setup some defaults for accounts that should be replicated to this RODC
    ctx.never_reveal_sid = [ "<SID=%s-%s>" % (ctx.domsid, security.DOMAIN_RID_RODC_DENY),
                             "<SID=%s>" % security.SID_BUILTIN_ADMINISTRATORS,
                             "<SID=%s>" % security.SID_BUILTIN_SERVER_OPERATORS,
                             "<SID=%s>" % security.SID_BUILTIN_BACKUP_OPERATORS,
                             "<SID=%s>" % security.SID_BUILTIN_ACCOUNT_OPERATORS ]
    ctx.reveal_sid = "<SID=%s-%s>" % (ctx.domsid, security.DOMAIN_RID_RODC_ALLOW)

    mysid = ctx.get_mysid()
    admin_dn = "<SID=%s>" % mysid
    ctx.managedby = admin_dn

    ctx.userAccountControl = (samba.dsdb.UF_WORKSTATION_TRUST_ACCOUNT |
                              samba.dsdb.UF_TRUSTED_TO_AUTHENTICATE_FOR_DELEGATION |
                              samba.dsdb.UF_PARTIAL_SECRETS_ACCOUNT)

    ctx.SPNs.extend([ "RestrictedKrbHost/%s" % ctx.myname,
                      "RestrictedKrbHost/%s" % ctx.dnshostname ])

    ctx.connection_dn = "CN=RODC Connection (FRS),%s" % ctx.ntds_dn
    ctx.secure_channel_type = misc.SEC_CHAN_RODC
    ctx.RODC = True
    ctx.replica_flags  =  (drsuapi.DRSUAPI_DRS_INIT_SYNC |
                           drsuapi.DRSUAPI_DRS_PER_SYNC |
                           drsuapi.DRSUAPI_DRS_GET_ANC |
                           drsuapi.DRSUAPI_DRS_NEVER_SYNCED |
                           drsuapi.DRSUAPI_DRS_SPECIAL_SECRET_PROCESSING |
                           drsuapi.DRSUAPI_DRS_GET_ALL_GROUP_MEMBERSHIP)
    ctx.domain_replica_flags = ctx.replica_flags
    if domain_critical_only:
        ctx.domain_replica_flags |= drsuapi.DRSUAPI_DRS_CRITICAL_ONLY

    ctx.do_join()


    print "Joined domain %s (SID %s) as an RODC" % (ctx.domain_name, ctx.domsid)


def join_DC(server=None, creds=None, lp=None, site=None, netbios_name=None,
            targetdir=None, domain=None, domain_critical_only=False):
    """join as a DC"""
    ctx = dc_join(server, creds, lp, site, netbios_name, targetdir, domain)

    lp.set("workgroup", ctx.domain_name)
    print("workgroup is %s" % ctx.domain_name)

    lp.set("realm", ctx.realm)
    print("realm is %s" % ctx.realm)

    ctx.userAccountControl = samba.dsdb.UF_SERVER_TRUST_ACCOUNT | samba.dsdb.UF_TRUSTED_FOR_DELEGATION

    ctx.SPNs.append('E3514235-4B06-11D1-AB04-00C04FC2DCD2/$NTDSGUID/%s' % ctx.dnsdomain)
    ctx.secure_channel_type = misc.SEC_CHAN_BDC

    ctx.replica_flags = (drsuapi.DRSUAPI_DRS_WRIT_REP |
                         drsuapi.DRSUAPI_DRS_INIT_SYNC |
                         drsuapi.DRSUAPI_DRS_PER_SYNC |
                         drsuapi.DRSUAPI_DRS_FULL_SYNC_IN_PROGRESS |
                         drsuapi.DRSUAPI_DRS_NEVER_SYNCED)
    ctx.domain_replica_flags = ctx.replica_flags
    if domain_critical_only:
        ctx.domain_replica_flags |= drsuapi.DRSUAPI_DRS_CRITICAL_ONLY

    ctx.do_join()
    print "Joined domain %s (SID %s) as a DC" % (ctx.domain_name, ctx.domsid)

def join_subdomain(server=None, creds=None, lp=None, site=None, netbios_name=None,
                   targetdir=None, parent_domain=None, dnsdomain=None, netbios_domain=None):
    """join as a DC"""
    ctx = dc_join(server, creds, lp, site, netbios_name, targetdir, parent_domain)
    ctx.subdomain = True
    ctx.parent_domain_name = ctx.domain_name
    ctx.domain_name = netbios_domain
    ctx.realm = dnsdomain
    ctx.parent_dnsdomain = ctx.dnsdomain
    ctx.parent_partition_dn = ctx.get_parent_partition_dn()
    ctx.dnsdomain = dnsdomain
    ctx.partition_dn = "CN=%s,CN=Partitions,%s" % (ctx.domain_name, ctx.config_dn)
    ctx.base_dn = samba.dn_from_dns_name(dnsdomain)
    ctx.domsid = str(security.random_sid())
    ctx.acct_dn = None
    ctx.dnshostname = "%s.%s" % (ctx.myname, ctx.dnsdomain)
    ctx.trustdom_pass = samba.generate_random_password(128, 128)

    ctx.userAccountControl = samba.dsdb.UF_SERVER_TRUST_ACCOUNT | samba.dsdb.UF_TRUSTED_FOR_DELEGATION

    ctx.SPNs.append('E3514235-4B06-11D1-AB04-00C04FC2DCD2/$NTDSGUID/%s' % ctx.dnsdomain)
    ctx.secure_channel_type = misc.SEC_CHAN_BDC

    ctx.replica_flags = (drsuapi.DRSUAPI_DRS_WRIT_REP |
                         drsuapi.DRSUAPI_DRS_INIT_SYNC |
                         drsuapi.DRSUAPI_DRS_PER_SYNC |
                         drsuapi.DRSUAPI_DRS_FULL_SYNC_IN_PROGRESS |
                         drsuapi.DRSUAPI_DRS_NEVER_SYNCED)
    ctx.domain_replica_flags = ctx.replica_flags

    ctx.do_join()
    print "Created domain %s (SID %s) as a DC" % (ctx.domain_name, ctx.domsid)
