#include "postgres.h"

#include "access/htup.h"
#include "access/htup_details.h"
#include "catalog/pg_proc.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "catalog/ag_catalog.h"
#include "utils/ag_func.h"

// checks that func_oid is of func_name function in ag_catalog
bool is_oid_ag_func(Oid func_oid, const char *func_name)
{
    HeapTuple proctup;
    Form_pg_proc proc;
    Oid nspid;
    const char *nspname;

    AssertArg(OidIsValid(func_oid));
    AssertArg(func_name);

    proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(func_oid));
    Assert(HeapTupleIsValid(proctup));
    proc = (Form_pg_proc)GETSTRUCT(proctup);
    if (strncmp(NameStr(proc->proname), func_name, NAMEDATALEN) != 0)
    {
        ReleaseSysCache(proctup);
        return false;
    }
    nspid = proc->pronamespace;
    ReleaseSysCache(proctup);

    nspname = get_namespace_name_or_temp(nspid);
    Assert(nspname);
    return (strcmp(nspname, "ag_catalog") == 0);
}

// gets the function OID that matches with func_name and argument types
Oid get_ag_func_oid(const char *func_name, const int nargs, ...)
{
    Oid oids[FUNC_MAX_ARGS];
    va_list ap;
    int i;
    oidvector *arg_types;
    Oid func_oid;

    AssertArg(func_name);
    AssertArg(nargs >= 0 && nargs <= FUNC_MAX_ARGS);

    va_start(ap, nargs);
    for (i = 0; i < nargs; i++)
        oids[i] = va_arg(ap, Oid);
    va_end(ap);

    arg_types = buildoidvector(oids, nargs);

    func_oid = GetSysCacheOid3(PROCNAMEARGSNSP, CStringGetDatum(func_name),
                               PointerGetDatum(arg_types),
                               ObjectIdGetDatum(ag_catalog_namespace_id()));
    if (!OidIsValid(func_oid))
    {
        ereport(ERROR, (errmsg_internal("function does not exist"),
                        errdetail_internal("%s(%d)", func_name, nargs)));
    }

    return func_oid;
}
