/* Authors: Martin Nagy <mnagy@redhat.com>
 *          Adam Tkac   <atkac@redhat.com>
 *
 * Copyright (C) 2008, 2009  Red Hat
 * see file 'COPYING' for use and warranty information
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; version 2 only
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include <isc/mem.h>
#include <isc/refcount.h>
#include <isc/util.h>

#include <dns/db.h>
#include <dns/rdatalist.h>
#include <dns/result.h>
#include <dns/types.h>

#include <string.h> /* For memcpy */

#include "ldap_helper.h"
#include "log.h"
#include "util.h"
#include "zone_manager.h"

#define LDAPDB_MAGIC			ISC_MAGIC('L', 'D', 'P', 'D')
#define VALID_LDAPDB(ldapdb) \
	((ldapdb) != NULL && (ldapdb)->common.impmagic == LDAPDB_MAGIC)

#define LDAPDBNODE_MAGIC		ISC_MAGIC('L', 'D', 'P', 'N')
#define VALID_LDAPDBNODE(ldapdbnode)	ISC_MAGIC_VALID(ldapdbnode, \
							LDAPDBNODE_MAGIC)

typedef struct {
	dns_db_t			common;
	isc_refcount_t			refs;
	isc_mutex_t			lock; /* convert to isc_rwlock_t ? */
	ldap_db_t			*ldap_db;
} ldapdb_t;

typedef struct {
	unsigned int			magic;
	isc_refcount_t			refs;
	dns_name_t			owner;
	ldapdb_rdatalist_t		rdatalist;
} ldapdbnode_t;

static int dummy;
static void *ldapdb_version = &dummy;

static void free_ldapdb(ldapdb_t *ldapdb);
static void detachnode(dns_db_t *db, dns_dbnode_t **targetp);


/* ldapdbnode_t functions */
static isc_result_t
ldapdbnode_create(isc_mem_t *mctx, dns_name_t *owner, ldapdbnode_t **nodep)
{
	ldapdbnode_t *node;
	isc_result_t result;

	REQUIRE(nodep != NULL && *nodep == NULL);

	node = isc_mem_get(mctx, sizeof(*node));
	if (node == NULL)
		return ISC_R_NOMEMORY;

	CHECK(isc_refcount_init(&node->refs, 1));

	dns_name_init(&node->owner, NULL);
	result = dns_name_dup(owner, mctx, &node->owner);
	if (result != ISC_R_SUCCESS)
		goto cleanup;

	node->magic = LDAPDBNODE_MAGIC;

	ISC_LIST_INIT(node->rdatalist);

	*nodep = node;

	return ISC_R_SUCCESS;

cleanup:
	isc_mem_put(mctx, node, sizeof(*node));
	return result;
}

/*
 * Functions.
 *
 * Most of them don't need db parameter but we are checking if it is valid.
 * Invalid db parameter indicates bug in code.
 */

static void
attach(dns_db_t *source, dns_db_t **targetp)
{
	ldapdb_t *ldapdb = (ldapdb_t *)source;

	REQUIRE(VALID_LDAPDB(ldapdb));

	isc_refcount_increment(&ldapdb->refs, NULL);
	*targetp = source;
}

static void
detach(dns_db_t **dbp)
{
	ldapdb_t *ldapdb = (ldapdb_t *)(*dbp);
	unsigned int refs;

	REQUIRE(VALID_LDAPDB(ldapdb));

	isc_refcount_decrement(&ldapdb->refs, &refs);

	if (refs == 0)
		free_ldapdb(ldapdb);

	*dbp = NULL;
}

static void
free_ldapdb(ldapdb_t *ldapdb)
{
	DESTROYLOCK(&ldapdb->lock);
	dns_name_free(&ldapdb->common.origin, ldapdb->common.mctx);
	isc_mem_putanddetach(&ldapdb->common.mctx, ldapdb, sizeof(*ldapdb));
}

static isc_result_t
beginload(dns_db_t *db, dns_addrdatasetfunc_t *addp, dns_dbload_t **dbloadp)
{

	UNUSED(db);
	UNUSED(addp);
	UNUSED(dbloadp);

	fatal_error("ldapdb: method beginload() should never be called");

	/* Not reached */
	return ISC_R_SUCCESS;
}

static isc_result_t
endload(dns_db_t *db, dns_dbload_t **dbloadp)
{

	UNUSED(db);
	UNUSED(dbloadp);

	fatal_error("ldapdb: method endload() should never be called");

	/* Not reached */
	return ISC_R_SUCCESS;
}

static isc_result_t
dump(dns_db_t *db, dns_dbversion_t *version, const char *filename,
     dns_masterformat_t masterformat)
{

	UNUSED(db);
	UNUSED(version);
	UNUSED(filename);
	UNUSED(masterformat);

	fatal_error("ldapdb: method dump() should never be called");

	/* Not reached */
	return ISC_R_SUCCESS;
}

static void
currentversion(dns_db_t *db, dns_dbversion_t **versionp)
{
	ldapdb_t *ldapdb = (ldapdb_t *)db;

	REQUIRE(VALID_LDAPDB(ldapdb));
	REQUIRE(versionp != NULL && *versionp == NULL);

	*versionp = ldapdb_version;
}

static isc_result_t
newversion(dns_db_t *db, dns_dbversion_t **versionp)
{
	ldapdb_t *ldapdb = (ldapdb_t *)db;

	REQUIRE(VALID_LDAPDB(ldapdb));
	REQUIRE(versionp != NULL && *versionp == NULL);

	*versionp = ldapdb_version;
	return ISC_R_SUCCESS;
}

static void
attachversion(dns_db_t *db, dns_dbversion_t *source,
	      dns_dbversion_t **targetp)
{
	ldapdb_t *ldapdb = (ldapdb_t *)db;

	REQUIRE(VALID_LDAPDB(ldapdb));
	REQUIRE(source == ldapdb_version);
	REQUIRE(targetp != NULL && *targetp == NULL);

	*targetp = ldapdb_version;
}

static void
closeversion(dns_db_t *db, dns_dbversion_t **versionp, isc_boolean_t commit)
{
	ldapdb_t *ldapdb = (ldapdb_t *)db;

	UNUSED(commit);

	REQUIRE(VALID_LDAPDB(ldapdb));
	REQUIRE(versionp != NULL && *versionp == ldapdb_version);

	*versionp = NULL;
}

/*
 * this is "extended" version of findnode which allows partial matches for
 * internal usage. Note that currently only exact matches work.
 */
static isc_result_t
findnode(dns_db_t *db, dns_name_t *name, isc_boolean_t create,
	 dns_dbnode_t **nodep)
{
	ldapdb_t *ldapdb = (ldapdb_t *) db;
	isc_result_t result;
	ldapdb_rdatalist_t rdatalist;
	ldapdbnode_t *node = NULL;

	log_func_enter_args("name=%s, create=%d", name->ndata, create);
	REQUIRE(VALID_LDAPDB(ldapdb));

	result = ldapdb_rdatalist_get(ldapdb->common.mctx, name, &rdatalist);
	INSIST(result != DNS_R_PARTIALMATCH); /* XXX notimp yet */

	/* If ldapdb_rdatalist_get has no memory node creation will fail as well */
	if (result == ISC_R_NOMEMORY)
		return ISC_R_NOMEMORY;

	if (create == ISC_FALSE) {
		/* No partial matches are allowed in this function */
		if (result == DNS_R_PARTIALMATCH) {
			result = ISC_R_NOTFOUND;
			goto cleanup;
		} else if (result != ISC_R_SUCCESS) {
			return result;
		}
	}

	result = ldapdbnode_create(ldapdb->common.mctx, name, &node);
	if (result != ISC_R_SUCCESS)
		goto cleanup;

	memcpy(&node->rdatalist, &rdatalist, sizeof(rdatalist));

	*nodep = node;

	log_func_exit_result(ISC_R_SUCCESS);

	return ISC_R_SUCCESS;

cleanup:
	ldapdb_rdatalist_destroy(ldapdb->common.mctx, &rdatalist);

	log_func_exit_result(result);

	return result;
}

/* XXX add support for DNAME redirection */
static isc_result_t
find(dns_db_t *db, dns_name_t *name, dns_dbversion_t *version,
     dns_rdatatype_t type, unsigned int options, isc_stdtime_t now,
     dns_dbnode_t **nodep, dns_name_t *foundname, dns_rdataset_t *rdataset,
     dns_rdataset_t *sigrdataset)
{
	ldapdb_t *ldapdb = (ldapdb_t *) db;
	isc_result_t result;
	ldapdbnode_t *node = NULL;
	dns_rdatalist_t *rdlist;
	isc_boolean_t is_cname = ISC_FALSE;
	ldapdb_rdatalist_t rdatalist;

	UNUSED(now);
	UNUSED(options);
	UNUSED(sigrdataset);

	log_func_enter();

	REQUIRE(VALID_LDAPDB(ldapdb));

	/* XXX not yet implemented */
	INSIST(type != dns_rdatatype_any);

	if (version != NULL) {
		REQUIRE(version == ldapdb_version);
	}

	result = ldapdb_rdatalist_get(ldapdb->common.mctx, name, &rdatalist);
	INSIST(result != DNS_R_PARTIALMATCH); /* XXX Not yet implemented */

	if (result != ISC_R_SUCCESS && result != DNS_R_PARTIALMATCH)
		return result;

	result = ldapdbnode_create(ldapdb->common.mctx, name, &node);
	if (result != ISC_R_SUCCESS) {
		ldapdb_rdatalist_destroy(ldapdb->common.mctx, &rdatalist);
		return result;
	}

	memcpy(&node->rdatalist, &rdatalist, sizeof(rdatalist));

	result = ldapdb_rdatalist_findrdatatype(&node->rdatalist, type,
						&rdlist);

	if (result != ISC_R_SUCCESS) {
		/* No exact rdtype match. Check CNAME */

		rdlist = HEAD(node->rdatalist);
		while (rdlist != NULL && rdlist->type != dns_rdatatype_cname)
			rdlist = NEXT(rdlist, link);

		/* CNAME was found */
		if (rdlist != NULL) {
			result = ISC_R_SUCCESS;
			is_cname = ISC_TRUE;
		}
	}

	if (result != ISC_R_SUCCESS) {
		result = DNS_R_NXRRSET;
		goto cleanup;
	}

	/* dns_rdatalist_tordataset returns success only */
	result = dns_rdatalist_tordataset(rdlist, rdataset);
	INSIST(result == ISC_R_SUCCESS);

	/* XXX currently we implemented only exact authoritative matches */
	result = dns_name_dupwithoffsets(name, ldapdb->common.mctx, foundname);
	if (result != ISC_R_SUCCESS)
		goto cleanup;

	*nodep = node;

	return (is_cname == ISC_TRUE) ? DNS_R_CNAME : ISC_R_SUCCESS;

cleanup:
	detachnode(db, ((dns_dbnode_t **) &node));
	return result;
}

static isc_result_t
findzonecut(dns_db_t *db, dns_name_t *name, unsigned int options,
	    isc_stdtime_t now, dns_dbnode_t **nodep, dns_name_t *foundname,
	    dns_rdataset_t *rdataset, dns_rdataset_t *sigrdataset)
{
	UNUSED(db);
	UNUSED(name);
	UNUSED(options);
	UNUSED(now);
	UNUSED(nodep);
	UNUSED(foundname);
	UNUSED(rdataset);
	UNUSED(sigrdataset);

	return ISC_R_NOTIMPLEMENTED;
}

static void
attachnode(dns_db_t *db, dns_dbnode_t *source, dns_dbnode_t **targetp)
{
	ldapdbnode_t *node = (ldapdbnode_t *) source;

	REQUIRE(VALID_LDAPDBNODE(node));

	UNUSED(db);

	isc_refcount_increment(&node->refs, NULL);
	*targetp = source;
}

static void
detachnode(dns_db_t *db, dns_dbnode_t **targetp)
{
	ldapdbnode_t *node = (ldapdbnode_t *)(*targetp);
	ldapdb_t *ldapdb = (ldapdb_t *) db;
	unsigned int refs;

	/*
	 * Don't check for db and targetp validity, it's done in
	 * dns_db_detachnode
	 */

	REQUIRE(VALID_LDAPDBNODE(node));

	isc_refcount_decrement(&node->refs, &refs);
	if (refs == 0) {
		ldapdb_rdatalist_destroy(ldapdb->common.mctx, &node->rdatalist);
		dns_name_free(&node->owner, ldapdb->common.mctx);
		isc_mem_put(ldapdb->common.mctx, node, sizeof(*node));
	}

	*targetp = NULL;
}

static isc_result_t
expirenode(dns_db_t *db, dns_dbnode_t *node, isc_stdtime_t now)
{
	UNUSED(db);
	UNUSED(node);
	UNUSED(now);

	return ISC_R_NOTIMPLEMENTED;
}

static void
printnode(dns_db_t *db, dns_dbnode_t *node, FILE *out)
{
	UNUSED(db);
	UNUSED(node);
	UNUSED(out);
}

static isc_result_t
createiterator(dns_db_t *db, unsigned int options,
	       dns_dbiterator_t **iteratorp)
{
	UNUSED(db);
	UNUSED(options);
	UNUSED(iteratorp);

	return ISC_R_NOTIMPLEMENTED;
}

static isc_result_t
findrdataset(dns_db_t *db, dns_dbnode_t *node, dns_dbversion_t *version,
	     dns_rdatatype_t type, dns_rdatatype_t covers, isc_stdtime_t now,
	     dns_rdataset_t *rdataset, dns_rdataset_t *sigrdataset)
{
	ldapdbnode_t *ldapdbnode = (ldapdbnode_t *) node;
	dns_rdatalist_t *rdlist = NULL;
	isc_result_t result;

	UNUSED(db);
	UNUSED(now);
	UNUSED(sigrdataset);

	REQUIRE(covers == 0); /* Only meaningful with DNSSEC capable DB*/
	REQUIRE(VALID_LDAPDBNODE(ldapdbnode));

	if (version != NULL) {
		REQUIRE(version == ldapdb_version);
	}

	result = ldapdb_rdatalist_findrdatatype(&ldapdbnode->rdatalist, type,
						&rdlist);
	if (result != ISC_R_SUCCESS)
		return result;

	dns_rdatalist_tordataset(rdlist, rdataset);
	return ISC_R_SUCCESS;
}

static isc_result_t
allrdatasets(dns_db_t *db, dns_dbnode_t *node, dns_dbversion_t *version,
	     isc_stdtime_t now, dns_rdatasetiter_t **iteratorp)
{
	UNUSED(db);
	UNUSED(node);
	UNUSED(version);
	UNUSED(now);
	UNUSED(iteratorp);

	return ISC_R_NOTIMPLEMENTED;
}

static isc_result_t
addrdataset(dns_db_t *db, dns_dbnode_t *node, dns_dbversion_t *version,
	    isc_stdtime_t now, dns_rdataset_t *rdataset, unsigned int options,
	    dns_rdataset_t *addedrdataset)
{
	UNUSED(db);
	UNUSED(node);
	UNUSED(version);
	UNUSED(now);
	UNUSED(rdataset);
	UNUSED(options);
	UNUSED(addedrdataset);

	return ISC_R_NOTIMPLEMENTED;
}

static isc_result_t
subtractrdataset(dns_db_t *db, dns_dbnode_t *node, dns_dbversion_t *version,
		 dns_rdataset_t *rdataset, unsigned int options,
		 dns_rdataset_t *newrdataset)
{
	UNUSED(db);
	UNUSED(node);
	UNUSED(version);
	UNUSED(rdataset);
	UNUSED(options);
	UNUSED(newrdataset);

	return ISC_R_NOTIMPLEMENTED;
}

static isc_result_t
deleterdataset(dns_db_t *db, dns_dbnode_t *node, dns_dbversion_t *version,
	       dns_rdatatype_t type, dns_rdatatype_t covers)
{
	UNUSED(db);
	UNUSED(node);
	UNUSED(version);
	UNUSED(type);
	UNUSED(covers);

	return ISC_R_NOTIMPLEMENTED;
}

static isc_boolean_t
issecure(dns_db_t *db)
{
	UNUSED(db);

	return ISC_R_NOTIMPLEMENTED;
}

static unsigned int
nodecount(dns_db_t *db)
{
	UNUSED(db);

	return ISC_R_NOTIMPLEMENTED;
}

static isc_boolean_t
ispersistent(dns_db_t *db)
{
	UNUSED(db);

	return ISC_R_NOTIMPLEMENTED;
}

static void
overmem(dns_db_t *db, isc_boolean_t overmem)
{
	UNUSED(db);
	UNUSED(overmem);
}

static void
settask(dns_db_t *db, isc_task_t *task)
{
	UNUSED(db);
	UNUSED(task);
}

static isc_result_t
getoriginnode(dns_db_t *db, dns_dbnode_t **nodep)
{
	UNUSED(db);
	UNUSED(nodep);

	return ISC_R_NOTIMPLEMENTED;
}

static void
transfernode(dns_db_t *db, dns_dbnode_t **sourcep, dns_dbnode_t **targetp)
{
	UNUSED(db);
	UNUSED(sourcep);
	UNUSED(targetp);
}

static isc_result_t
getnsec3parameters(dns_db_t *db, dns_dbversion_t *version, dns_hash_t *hash,
		   isc_uint8_t *flags, isc_uint16_t *iterations,
		   unsigned char *salt, size_t *salt_len)
{
	UNUSED(db);
	UNUSED(version);
	UNUSED(hash);
	UNUSED(flags);
	UNUSED(iterations);
	UNUSED(salt);
	UNUSED(salt_len);

	return ISC_R_NOTIMPLEMENTED;
}

static isc_result_t
findnsec3node(dns_db_t *db, dns_name_t *name, isc_boolean_t create,
	      dns_dbnode_t **nodep)
{
	UNUSED(db);
	UNUSED(name);
	UNUSED(create);
	UNUSED(nodep);

	return ISC_R_NOTIMPLEMENTED;
}

static isc_result_t
setsigningtime(dns_db_t *db, dns_rdataset_t *rdataset, isc_stdtime_t resign)
{
	UNUSED(db);
	UNUSED(rdataset);
	UNUSED(resign);

	return ISC_R_NOTIMPLEMENTED;
}

static isc_result_t
getsigningtime(dns_db_t *db, dns_rdataset_t *rdataset, dns_name_t *name)
{
	UNUSED(db);
	UNUSED(rdataset);
	UNUSED(name);

	return ISC_R_NOTIMPLEMENTED;
}

static void
resigned(dns_db_t *db, dns_rdataset_t *rdataset, dns_dbversion_t *version)
{
	UNUSED(db);
	UNUSED(rdataset);
	UNUSED(version);
}

static isc_boolean_t
isdnssec(dns_db_t *db)
{
	UNUSED(db);

	return ISC_R_NOTIMPLEMENTED;
}

static dns_stats_t *
getrrsetstats(dns_db_t *db)
{
	UNUSED(db);

	return NULL;
}

static dns_dbmethods_t ldapdb_methods = {
	attach,
	detach,
	beginload,
	endload,
	dump,
	currentversion,
	newversion,
	attachversion,
	closeversion,
	findnode,
	find,
	findzonecut,
	attachnode,
	detachnode,
	expirenode,
	printnode,
	createiterator,
	findrdataset,
	allrdatasets,
	addrdataset,
	subtractrdataset,
	deleterdataset,
	issecure,
	nodecount,
	ispersistent,
	overmem,
	settask,
	getoriginnode,
	transfernode,
	getnsec3parameters,
	findnsec3node,
	setsigningtime,
	getsigningtime,
	resigned,
	isdnssec,
	getrrsetstats
};

static isc_result_t
ldapdb_create(isc_mem_t *mctx, dns_name_t *name, dns_dbtype_t type,
	      dns_rdataclass_t rdclass, unsigned int argc, char *argv[],
	      void *driverarg, dns_db_t **dbp)
{
	ldapdb_t *ldapdb;
	isc_result_t result;

	UNUSED(driverarg); /* Currently we don't need any data */

	/* Database implementation name and name pointing to ldap_db_t */
	REQUIRE(argc > 0);

	REQUIRE(type == dns_dbtype_zone);
	REQUIRE(rdclass == dns_rdataclass_in);
	REQUIRE(dbp != NULL && *dbp == NULL);

	ldapdb = isc_mem_get(mctx, sizeof(*ldapdb));
	if (ldapdb == NULL)
		return ISC_R_NOMEMORY;

	ldapdb->common.methods = &ldapdb_methods;
	ldapdb->common.attributes = 0;
	ldapdb->common.rdclass = rdclass;

	dns_name_init(&ldapdb->common.origin, NULL);
	result = dns_name_dupwithoffsets(name, mctx, &ldapdb->common.origin);
	if (result != ISC_R_SUCCESS)
		goto clean_ldapdb;

	isc_ondestroy_init(&ldapdb->common.ondest);
	ldapdb->common.mctx = NULL;
	isc_mem_attach(mctx, &ldapdb->common.mctx);

	result = isc_mutex_init(&ldapdb->lock);
	if (result != ISC_R_SUCCESS)
		goto clean_origin;

	result = isc_refcount_init(&ldapdb->refs, 1);
	if (result != ISC_R_SUCCESS)
		goto clean_lock;

	result = manager_get_ldap_db(argv[0], &ldapdb->ldap_db);
	if (result != ISC_R_SUCCESS)
		goto clean_lock;

	ldapdb->common.magic = DNS_DB_MAGIC;
	ldapdb->common.impmagic = LDAPDB_MAGIC;

	*dbp = (dns_db_t *)ldapdb;

	return ISC_R_SUCCESS;

clean_lock:
	DESTROYLOCK(&ldapdb->lock);
clean_origin:
	dns_name_free(&ldapdb->common.origin, mctx);
clean_ldapdb:
	isc_mem_putanddetach(&ldapdb->common.mctx, ldapdb, sizeof(*ldapdb));

	return result;
}

static dns_dbimplementation_t *ldapdb_imp;
const char *ldapdb_impname = "dynamic-ldap";


isc_result_t
dynamic_driver_init(isc_mem_t *mctx, const char *name, const char * const *argv,
		    dns_view_t *view, dns_zonemgr_t *zmgr)
{
	isc_result_t result;
	ldap_db_t *ldap_db;

	REQUIRE(mctx != NULL);
	REQUIRE(name != NULL);
	REQUIRE(argv != NULL);
	REQUIRE(view != NULL);

	ldap_db = NULL;

	log_debug(2, "Registering dynamic ldap driver for %s.", name);

	/* Test argv. */
	int i = 0;
	while (argv[i] != NULL) {
		log_debug(2, "Arg: %s", argv[i]);
		i++;
	}

	/* Register new DNS DB implementation. */
	result = dns_db_register(ldapdb_impname, &ldapdb_create, NULL, mctx,
				 &ldapdb_imp);
	if (result == ISC_R_EXISTS)
		result = ISC_R_SUCCESS;

	if (result != ISC_R_SUCCESS)
		return result;

	CHECK(new_ldap_db(mctx, view, &ldap_db, argv));
	CHECK(manager_add_db_instance(mctx, name, ldap_db, zmgr));

	/*
	 * XXX now fetch all zones and initialize ldap zone manager
	 * (periodically check for new zones)
	 * - manager has to share server zonemgr (ns_g_server->zonemgr)
	 *
	 * XXX manager has to this this for each zone:
	 * - dns_zone_create
	 * - dns_zone_setorigin
	 * - dns_zone_setview
	 * - dns_zone_setacache (probably not needed)
	 * - dns_zone_setclass
	 * - dns_zone_settype
	 * - dns_zone_setdbtype (note: pass all connection arguments etc here -
	 *   will be used by ldapdb_create)
	 * - continue as in bin/server.c - ns_zone_configure()
	 * - dns_zonemgr_managezone
	 *
	 * zone has to be bind-ed to specified view:
	 * - dns_view_findzone (check if zone already exists)
	 * - dns_view_addzone
	 */

	return ISC_R_SUCCESS;

cleanup:
	if (ldap_db != NULL)
		destroy_ldap_db(&ldap_db);

	return result;
}

void
dynamic_driver_destroy(void)
{
	dns_db_unregister(&ldapdb_imp);
	destroy_manager();
}
