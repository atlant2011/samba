#!/usr/bin/env python
#
# Samba4 AD database checker
#
# Copyright (C) Andrew Tridgell 2011
# Copyright (C) Matthieu Patou <mat@matws.net> 2011
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

import ldb
from samba import dsdb
from samba import common
from samba.dcerpc import misc
from samba.ndr import ndr_unpack
from samba.dcerpc import drsblobs


class dsdb_DN(object):
    '''a class to manipulate DN components'''

    def __init__(self, samdb, dnstring, syntax_oid):
        if syntax_oid in [ dsdb.DSDB_SYNTAX_BINARY_DN, dsdb.DSDB_SYNTAX_STRING_DN ]:
            colons = dnstring.split(':')
            if len(colons) < 4:
                raise Exception("invalid DN prefix")
            prefix_len = 4 + len(colons[1]) + int(colons[1])
            self.prefix = dnstring[0:prefix_len]
            self.dnstring = dnstring[prefix_len:]
        else:
            self.dnstring = dnstring
            self.prefix = ''
        try:
            self.dn = ldb.Dn(samdb, self.dnstring)
        except Exception, msg:
            print("ERROR: bad DN string '%s'" % self.dnstring)
            raise

    def __str__(self):
        return self.prefix + str(self.dn.extended_str(mode=1))

class dbcheck(object):
    """check a SAM database for errors"""

    def __init__(self, samdb, samdb_schema=None, verbose=False, fix=False, yes=False, quiet=False):
        self.samdb = samdb
        self.dict_oid_name = None
        self.samdb_schema = (samdb_schema or samdb)
        self.verbose = verbose
        self.fix = fix
        self.yes = yes
        self.quiet = quiet
        self.remove_all_unknown_attributes = False
        self.remove_all_empty_attributes = False
        self.fix_all_normalisation = False
        self.fix_all_DN_GUIDs = False
        self.remove_all_deleted_DN_links = False
        self.fix_all_target_mismatch = False
        self.fix_all_metadata = False
        self.fix_all_missing_backlinks = False
        self.fix_all_orphaned_backlinks = False

    def check_database(self, DN=None, scope=ldb.SCOPE_SUBTREE, controls=[], attrs=['*']):
        '''perform a database check, returning the number of errors found'''

        res = self.samdb.search(base=DN, scope=scope, attrs=['dn'], controls=controls)
        self.report('Checking %u objects' % len(res))
        error_count = 0

        for object in res:
            error_count += self.check_object(object.dn, attrs=attrs)

        if DN is None:
            error_count += self.check_rootdse()

        if error_count != 0 and not self.fix:
            self.report("Please use --fix to fix these errors")


        self.report('Checked %u objects (%u errors)' % (len(res), error_count))

        return error_count


    def report(self, msg):
        '''print a message unless quiet is set'''
        if not self.quiet:
            print(msg)


    ################################################################
    # a local confirm function that obeys the --fix and --yes options
    def confirm(self, msg, allow_all=False, forced=False):
        '''confirm a change'''
        if not self.fix:
            return False
        if self.quiet:
            return self.yes
        if self.yes:
            forced = True
        return common.confirm(msg, forced=forced, allow_all=allow_all)

    ################################################################
    # a local confirm function with support for 'all'
    def confirm_all(self, msg, all_attr):
        '''confirm a change with support for "all" '''
        if not self.fix:
            return False
        if self.quiet:
            return self.yes
        if getattr(self, all_attr) == 'NONE':
            return False
        if getattr(self, all_attr) == 'ALL':
            forced = True
        else:
            forced = self.yes
        c = common.confirm(msg, forced=forced, allow_all=True)
        if c == 'ALL':
            setattr(self, all_attr, 'ALL')
            return True
        if c == 'NONE':
            setattr(self, all_attr, 'NONE')
            return True
        return c


    def do_modify(self, m, controls, msg, validate=True):
        '''perform a modify with optional verbose output'''
        if self.verbose:
            self.report(self.samdb.write_ldif(m, ldb.CHANGETYPE_MODIFY))
        try:
            self.samdb.modify(m, controls=controls, validate=validate)
        except Exception, err:
            self.report("%s : %s" % (msg, err))
            return False
        return True


    ################################################################
    # handle empty attributes
    def err_empty_attribute(self, dn, attrname):
        '''fix empty attributes'''
        self.report("ERROR: Empty attribute %s in %s" % (attrname, dn))
        if not self.confirm_all('Remove empty attribute %s from %s?' % (attrname, dn), 'remove_all_empty_attributes'):
            self.report("Not fixing empty attribute %s" % attrname)
            return

        m = ldb.Message()
        m.dn = dn
        m[attrname] = ldb.MessageElement('', ldb.FLAG_MOD_DELETE, attrname)
        if self.do_modify(m, ["relax:0", "show_recycled:1"],
                          "Failed to remove empty attribute %s" % attrname, validate=False):
            self.report("Removed empty attribute %s" % attrname)


    ################################################################
    # handle normalisation mismatches
    def err_normalise_mismatch(self, dn, attrname, values):
        '''fix attribute normalisation errors'''
        self.report("ERROR: Normalisation error for attribute %s in %s" % (attrname, dn))
        mod_list = []
        for val in values:
            normalised = self.samdb.dsdb_normalise_attributes(self.samdb_schema, attrname, [val])
            if len(normalised) != 1:
                self.report("Unable to normalise value '%s'" % val)
                mod_list.append((val, ''))
            elif (normalised[0] != val):
                self.report("value '%s' should be '%s'" % (val, normalised[0]))
                mod_list.append((val, normalised[0]))
        if not self.confirm_all('Fix normalisation for %s from %s?' % (attrname, dn), 'fix_all_normalisation'):
            self.report("Not fixing attribute %s" % attrname)
            return

        m = ldb.Message()
        m.dn = dn
        for i in range(0, len(mod_list)):
            (val, nval) = mod_list[i]
            m['value_%u' % i] = ldb.MessageElement(val, ldb.FLAG_MOD_DELETE, attrname)
            if nval != '':
                m['normv_%u' % i] = ldb.MessageElement(nval, ldb.FLAG_MOD_ADD, attrname)

        if self.do_modify(m, ["relax:0", "show_recycled:1"],
                          "Failed to normalise attribute %s" % attrname,
                          validate=False):
            self.report("Normalised attribute %s" % attrname)

    def is_deleted_objects_dn(self, dsdb_dn):
        '''see if a dsdb_DN is the special Deleted Objects DN'''
        return dsdb_dn.prefix == "B:32:18E2EA80684F11D2B9AA00C04F79F805:"


    ################################################################
    # handle a missing GUID extended DN component
    def err_incorrect_dn_GUID(self, dn, attrname, val, dsdb_dn, errstr):
        self.report("ERROR: %s component for %s in object %s - %s" % (errstr, attrname, dn, val))
        controls=["extended_dn:1:1", "show_recycled:1"]
        try:
            res = self.samdb.search(base=str(dsdb_dn.dn), scope=ldb.SCOPE_BASE,
                                    attrs=[], controls=controls)
        except ldb.LdbError, (enum, estr):
            self.report("unable to find object for DN %s - cannot fix (%s)" % (dsdb_dn.dn, estr))
            return
        if len(res) == 0:
            self.report("unable to find object for DN %s - cannot fix" % dsdb_dn.dn)
            return
        dsdb_dn.dn = res[0].dn

        if not self.confirm_all('Change DN to %s?' % str(dsdb_dn), 'fix_all_DN_GUIDs'):
            self.report("Not fixing %s" % errstr)
            return
        m = ldb.Message()
        m.dn = dn
        m['old_value'] = ldb.MessageElement(val, ldb.FLAG_MOD_DELETE, attrname)
        m['new_value'] = ldb.MessageElement(str(dsdb_dn), ldb.FLAG_MOD_ADD, attrname)

        if self.do_modify(m, ["show_recycled:1"],
                          "Failed to fix %s on attribute %s" % (errstr, attrname)):
            self.report("Fixed %s on attribute %s" % (errstr, attrname))


    ################################################################
    # handle a DN pointing to a deleted object
    def err_deleted_dn(self, dn, attrname, val, dsdb_dn, correct_dn):
        self.report("ERROR: target DN is deleted for %s in object %s - %s" % (attrname, dn, val))
        self.report("Target GUID points at deleted DN %s" % correct_dn)
        if not self.confirm_all('Remove DN?', 'remove_all_deleted_DN_links'):
            self.report("Not removing")
            return
        m = ldb.Message()
        m.dn = dn
        m['old_value'] = ldb.MessageElement(val, ldb.FLAG_MOD_DELETE, attrname)
        if self.do_modify(m, ["show_recycled:1"],
                          "Failed to remove deleted DN attribute %s" % attrname):
            self.report("Removed deleted DN on attribute %s" % attrname)


    ################################################################
    # handle a DN string being incorrect
    def err_dn_target_mismatch(self, dn, attrname, val, dsdb_dn, correct_dn, errstr):
        self.report("ERROR: incorrect DN string component for %s in object %s - %s" % (attrname, dn, val))
        dsdb_dn.dn = correct_dn

        if not self.confirm_all('Change DN to %s?' % str(dsdb_dn), 'fix_all_target_mismatch'):
            self.report("Not fixing %s" % errstr)
            return
        m = ldb.Message()
        m.dn = dn
        m['old_value'] = ldb.MessageElement(str(ldb.Dn(self.samdb, val)), ldb.FLAG_MOD_DELETE, attrname)
        m['new_value'] = ldb.MessageElement(str(dsdb_dn), ldb.FLAG_MOD_ADD, attrname)
        if self.do_modify(m, ["show_recycled:1"],
                          "Failed to fix incorrect DN string on attribute %s" % attrname):
            self.report("Fixed incorrect DN string on attribute %s" % (attrname))

    ################################################################
    # handle an unknown attribute error
    def err_unknown_attribute(self, obj, attrname):
        '''handle an unknown attribute error'''
        self.report("ERROR: unknown attribute '%s' in %s" % (attrname, obj.dn))
        if not self.confirm_all('Remove unknown attribute %s' % attrname, 'remove_all_unknown_attributes'):
            self.report("Not removing %s" % attrname)
            return
        m = ldb.Message()
        m.dn = obj.dn
        m['old_value'] = ldb.MessageElement([], ldb.FLAG_MOD_DELETE, attrname)
        if self.do_modify(m, ["relax:0", "show_recycled:1"],
                          "Failed to remove unknown attribute %s" % attrname):
            self.report("Removed unknown attribute %s" % (attrname))


    ################################################################
    # handle a missing backlink
    def err_missing_backlink(self, obj, attrname, val, backlink_name, target_dn):
        '''handle a missing backlink value'''
        self.report("ERROR: missing backlink attribute '%s' in %s for link %s in %s" % (backlink_name, target_dn, attrname, obj.dn))
        if not self.confirm_all('Fix missing backlink %s' % backlink_name, 'fix_all_missing_backlinks'):
            self.report("Not fixing missing backlink %s" % backlink_name)
            return
        m = ldb.Message()
        m.dn = obj.dn
        m['old_value'] = ldb.MessageElement(val, ldb.FLAG_MOD_DELETE, attrname)
        m['new_value'] = ldb.MessageElement(val, ldb.FLAG_MOD_ADD, attrname)
        if self.do_modify(m, ["show_recycled:1"],
                          "Failed to fix missing backlink %s" % backlink_name):
            self.report("Fixed missing backlink %s" % (backlink_name))


    ################################################################
    # handle a orphaned backlink
    def err_orphaned_backlink(self, obj, attrname, val, link_name, target_dn):
        '''handle a orphaned backlink value'''
        self.report("ERROR: orphaned backlink attribute '%s' in %s for link %s in %s" % (attrname, obj.dn, link_name, target_dn))
        if not self.confirm_all('Remove orphaned backlink %s' % link_name, 'fix_all_orphaned_backlinks'):
            self.report("Not removing orphaned backlink %s" % link_name)
            return
        m = ldb.Message()
        m.dn = obj.dn
        m['value'] = ldb.MessageElement(val, ldb.FLAG_MOD_DELETE, attrname)
        if self.do_modify(m, ["show_recycled:1", "relax:0"],
                          "Failed to fix orphaned backlink %s" % link_name):
            self.report("Fixed orphaned backlink %s" % (link_name))


    ################################################################
    # specialised checking for a dn attribute
    def check_dn(self, obj, attrname, syntax_oid):
        '''check a DN attribute for correctness'''
        error_count = 0
        for val in obj[attrname]:
            dsdb_dn = dsdb_DN(self.samdb, val, syntax_oid)

            # all DNs should have a GUID component
            guid = dsdb_dn.dn.get_extended_component("GUID")
            if guid is None:
                error_count += 1
                self.err_incorrect_dn_GUID(obj.dn, attrname, val, dsdb_dn, "missing GUID")
                continue

            guidstr = str(misc.GUID(guid))

            attrs=['isDeleted']
            linkkID = self.samdb_schema.get_linkId_from_lDAPDisplayName(attrname)
            reverse_link_name = self.samdb_schema.get_backlink_from_lDAPDisplayName(attrname)
            if reverse_link_name is not None:
                attrs.append(reverse_link_name)

            # check its the right GUID
            try:
                res = self.samdb.search(base="<GUID=%s>" % guidstr, scope=ldb.SCOPE_BASE,
                                        attrs=attrs, controls=["extended_dn:1:1", "show_recycled:1"])
            except ldb.LdbError, (enum, estr):
                error_count += 1
                self.err_incorrect_dn_GUID(obj.dn, attrname, val, dsdb_dn, "incorrect GUID")
                continue

            # now we have two cases - the source object might or might not be deleted
            is_deleted = 'isDeleted' in obj and obj['isDeleted'][0].upper() == 'TRUE'
            target_is_deleted = 'isDeleted' in res[0] and res[0]['isDeleted'][0].upper() == 'TRUE'

            # the target DN is not allowed to be deleted, unless the target DN is the
            # special Deleted Objects container
            if target_is_deleted and not is_deleted and not self.is_deleted_objects_dn(dsdb_dn):
                error_count += 1
                self.err_deleted_dn(obj.dn, attrname, val, dsdb_dn, res[0].dn)
                continue

            # check the DN matches in string form
            if res[0].dn.extended_str() != dsdb_dn.dn.extended_str():
                error_count += 1
                self.err_dn_target_mismatch(obj.dn, attrname, val, dsdb_dn,
                                            res[0].dn, "incorrect string version of DN")
                continue

            # check the reverse_link is correct if there should be one
            if reverse_link_name is not None:
                match_count = 0
                if reverse_link_name in res[0]:
                    for v in res[0][reverse_link_name]:
                        if v == obj.dn.extended_str():
                            match_count += 1
                if match_count != 1:
                    error_count += 1
                    if linkkID & 1:
                        self.err_orphaned_backlink(obj, attrname, val, reverse_link_name, dsdb_dn.dn)
                    else:
                        self.err_missing_backlink(obj, attrname, val, reverse_link_name, dsdb_dn.dn)
                    continue

        return error_count


    def process_metadata(self, val):
        '''Read metadata properties and list attributes in it'''

        list_att = []

        repl = ndr_unpack(drsblobs.replPropertyMetaDataBlob, str(val))
        obj = repl.ctr

        for o in repl.ctr.array:
            att = self.samdb_schema.get_lDAPDisplayName_by_attid(o.attid)
            list_att.append(att.lower())

        return list_att


    def fix_metadata(self, dn, attr):
        '''re-write replPropertyMetaData elements for a single attribute for a
        object. This is used to fix missing replPropertyMetaData elements'''
        res = self.samdb.search(base = dn, scope=ldb.SCOPE_BASE, attrs = [attr],
                                controls = ["search_options:1:2", "show_recycled:1"])
        msg = res[0]
        nmsg = ldb.Message()
        nmsg.dn = dn
        nmsg[attr] = ldb.MessageElement(msg[attr], ldb.FLAG_MOD_REPLACE, attr)
        if self.do_modify(nmsg, ["relax:0", "provision:0", "show_recycled:1"],
                          "Failed to fix metadata for attribute %s" % attr):
            self.report("Fixed metadata for attribute %s" % attr)


    ################################################################
    # check one object - calls to individual error handlers above
    def check_object(self, dn, attrs=['*']):
        '''check one object'''
        if self.verbose:
            self.report("Checking object %s" % dn)
        if '*' in attrs:
            attrs.append("replPropertyMetaData")

        res = self.samdb.search(base=dn, scope=ldb.SCOPE_BASE,
                                controls=["extended_dn:1:1", "show_recycled:1"],
                                attrs=attrs)
        if len(res) != 1:
            self.report("Object %s disappeared during check" % dn)
            return 1
        obj = res[0]
        error_count = 0
        list_attrs_from_md = []
        list_attrs_seen = []
        got_repl_property_meta_data = False

        for attrname in obj:
            if attrname == 'dn':
                continue

            if str(attrname).lower() == 'replpropertymetadata':
                list_attrs_from_md = self.process_metadata(obj[attrname])
                got_repl_property_meta_data = True
                continue


            # check for empty attributes
            for val in obj[attrname]:
                if val == '':
                    self.err_empty_attribute(dn, attrname)
                    error_count += 1
                    continue

            # get the syntax oid for the attribute, so we can can have
            # special handling for some specific attribute types
            try:
                syntax_oid = self.samdb_schema.get_syntax_oid_from_lDAPDisplayName(attrname)
            except Exception, msg:
                self.err_unknown_attribute(obj, attrname)
                error_count += 1
                continue

            flag = self.samdb_schema.get_systemFlags_from_lDAPDisplayName(attrname)
            if (not flag & dsdb.DS_FLAG_ATTR_NOT_REPLICATED
                and not flag & dsdb.DS_FLAG_ATTR_IS_CONSTRUCTED
                and not self.samdb_schema.get_linkId_from_lDAPDisplayName(attrname)):
                list_attrs_seen.append(str(attrname).lower())

            if syntax_oid in [ dsdb.DSDB_SYNTAX_BINARY_DN, dsdb.DSDB_SYNTAX_OR_NAME,
                               dsdb.DSDB_SYNTAX_STRING_DN, ldb.SYNTAX_DN ]:
                # it's some form of DN, do specialised checking on those
                error_count += self.check_dn(obj, attrname, syntax_oid)

            # check for incorrectly normalised attributes
            for val in obj[attrname]:
                normalised = self.samdb.dsdb_normalise_attributes(self.samdb_schema, attrname, [val])
                if len(normalised) != 1 or normalised[0] != val:
                    self.err_normalise_mismatch(dn, attrname, obj[attrname])
                    error_count += 1
                    break

        show_dn = True
        if got_repl_property_meta_data:
            for att in list_attrs_seen:
                if not att in list_attrs_from_md:
                    if show_dn:
                        self.report("On object %s" % dn)
                        show_dn = False
                    error_count += 1
                    self.report("ERROR: Attribute %s not present in replication metadata" % att)
                    if not self.confirm_all("Fix missing replPropertyMetaData element '%s'" % att, 'fix_all_metadata'):
                        self.report("Not fixing missing replPropertyMetaData element '%s'" % att)
                        continue
                    self.fix_metadata(dn, att)

        return error_count

    ################################################################
    # check special @ROOTDSE attributes
    def check_rootdse(self):
        '''check the @ROOTDSE special object'''
        dn = ldb.Dn(self.samdb, '@ROOTDSE')
        if self.verbose:
            self.report("Checking object %s" % dn)
        res = self.samdb.search(base=dn, scope=ldb.SCOPE_BASE)
        if len(res) != 1:
            self.report("Object %s disappeared during check" % dn)
            return 1
        obj = res[0]
        error_count = 0

        # check that the dsServiceName is in GUID form
        if not 'dsServiceName' in obj:
            self.report('ERROR: dsServiceName missing in @ROOTDSE')
            return error_count+1

        if not obj['dsServiceName'][0].startswith('<GUID='):
            self.report('ERROR: dsServiceName not in GUID form in @ROOTDSE')
            error_count += 1
            if not self.confirm('Change dsServiceName to GUID form?'):
                return error_count
            res = self.samdb.search(base=ldb.Dn(self.samdb, obj['dsServiceName'][0]),
                                    scope=ldb.SCOPE_BASE, attrs=['objectGUID'])
            guid_str = str(ndr_unpack(misc.GUID, res[0]['objectGUID'][0]))
            m = ldb.Message()
            m.dn = dn
            m['dsServiceName'] = ldb.MessageElement("<GUID=%s>" % guid_str,
                                                    ldb.FLAG_MOD_REPLACE, 'dsServiceName')
            if self.do_modify(m, [], "Failed to change dsServiceName to GUID form", validate=False):
                self.report("Changed dsServiceName to GUID form")
        return error_count


    ###############################################
    # re-index the database
    def reindex_database(self):
        '''re-index the whole database'''
        m = ldb.Message()
        m.dn = ldb.Dn(self.samdb, "@ATTRIBUTES")
        m['add']    = ldb.MessageElement('NONE', ldb.FLAG_MOD_ADD, 'force_reindex')
        m['delete'] = ldb.MessageElement('NONE', ldb.FLAG_MOD_DELETE, 'force_reindex')
        return self.do_modify(m, [], 're-indexed database', validate=False)
