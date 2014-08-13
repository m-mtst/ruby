/*
 *  (c) 1995 Microsoft Corporation. All rights reserved.
 *  Developed by ActiveWare Internet Corp., http://www.ActiveWare.com
 *
 *  Other modifications Copyright (c) 1997, 1998 by Gurusamy Sarathy
 *  <gsar@umich.edu> and Jan Dubois <jan.dubois@ibm.net>
 *
 *  You may distribute under the terms of either the GNU General Public
 *  License or the Artistic License, as specified in the README file
 *  of the Perl distribution.
 *
 */

/*
  modified for win32ole (ruby) by Masaki.Suketa <masaki.suketa@nifty.ne.jp>
 */

#include "win32ole.h"

/*
 * unfortunately IID_IMultiLanguage2 is not included in any libXXX.a
 * in Cygwin(mingw32).
 */
#if defined(__CYGWIN__) ||  defined(__MINGW32__)
#undef IID_IMultiLanguage2
const IID IID_IMultiLanguage2 = {0xDCCFC164, 0x2B38, 0x11d2, {0xB7, 0xEC, 0x00, 0xC0, 0x4F, 0x8F, 0x5D, 0x9A}};
#endif

#define OLEData_Get_Struct(obj, pole) {\
    Data_Get_Struct(obj, struct oledata, pole);\
    if(!pole->pDispatch) {\
        rb_raise(rb_eRuntimeError, "failed to get Dispatch Interface");\
    }\
}


#define WIN32OLE_VERSION "1.7.7"

typedef HRESULT (STDAPICALLTYPE FNCOCREATEINSTANCEEX)
    (REFCLSID, IUnknown*, DWORD, COSERVERINFO*, DWORD, MULTI_QI*);

typedef HWND (WINAPI FNHTMLHELP)(HWND hwndCaller, LPCSTR pszFile,
                                 UINT uCommand, DWORD dwData);
typedef BOOL (FNENUMSYSEMCODEPAGES) (CODEPAGE_ENUMPROC, DWORD);
typedef struct {
    struct IEventSinkVtbl * lpVtbl;
} IEventSink, *PEVENTSINK;

typedef struct IEventSinkVtbl IEventSinkVtbl;

struct IEventSinkVtbl {
    STDMETHOD(QueryInterface)(
        PEVENTSINK,
        REFIID,
        LPVOID *);
    STDMETHOD_(ULONG, AddRef)(PEVENTSINK);
    STDMETHOD_(ULONG, Release)(PEVENTSINK);

    STDMETHOD(GetTypeInfoCount)(
        PEVENTSINK,
        UINT *);
    STDMETHOD(GetTypeInfo)(
        PEVENTSINK,
        UINT,
        LCID,
        ITypeInfo **);
    STDMETHOD(GetIDsOfNames)(
        PEVENTSINK,
        REFIID,
        OLECHAR **,
        UINT,
        LCID,
        DISPID *);
    STDMETHOD(Invoke)(
        PEVENTSINK,
        DISPID,
        REFIID,
        LCID,
        WORD,
        DISPPARAMS *,
        VARIANT *,
        EXCEPINFO *,
        UINT *);
};

typedef struct tagIEVENTSINKOBJ {
    IEventSinkVtbl *lpVtbl;
    DWORD m_cRef;
    IID m_iid;
    int m_event_id;
    ITypeInfo *pTypeInfo;
}IEVENTSINKOBJ, *PIEVENTSINKOBJ;

VALUE cWIN32OLE;
VALUE cWIN32OLE_EVENT;
VALUE cWIN32OLE_RECORD;

static VALUE ary_ole_event;
static ID id_events;

#if defined(RB_THREAD_SPECIFIC) && (defined(__CYGWIN__) || defined(__MINGW32__))
static RB_THREAD_SPECIFIC BOOL g_ole_initialized;
# define g_ole_initialized_init() ((void)0)
# define g_ole_initialized_set(val) (g_ole_initialized = (val))
#else
static volatile DWORD g_ole_initialized_key = TLS_OUT_OF_INDEXES;
# define g_ole_initialized (BOOL)TlsGetValue(g_ole_initialized_key)
# define g_ole_initialized_init() (g_ole_initialized_key = TlsAlloc())
# define g_ole_initialized_set(val) TlsSetValue(g_ole_initialized_key, (void*)(val))
#endif

static BOOL g_uninitialize_hooked = FALSE;
static BOOL g_cp_installed = FALSE;
static BOOL g_lcid_installed = FALSE;
static HINSTANCE ghhctrl = NULL;
static HINSTANCE gole32 = NULL;
static FNCOCREATEINSTANCEEX *gCoCreateInstanceEx = NULL;
static VALUE com_hash;
static IDispatchVtbl com_vtbl;
static UINT cWIN32OLE_cp = CP_ACP;
static rb_encoding *cWIN32OLE_enc;
static UINT g_cp_to_check = CP_ACP;
static char g_lcid_to_check[8 + 1];
static VARTYPE g_nil_to = VT_ERROR;
static st_table *enc2cp_table;
static IMessageFilterVtbl message_filter;
static IMessageFilter imessage_filter = { &message_filter };
static IMessageFilter* previous_filter;

#if defined(HAVE_TYPE_IMULTILANGUAGE2)
static IMultiLanguage2 *pIMultiLanguage = NULL;
#elif defined(HAVE_TYPE_IMULTILANGUAGE)
static IMultiLanguage *pIMultiLanguage = NULL;
#else
#define pIMultiLanguage NULL /* dummy */
#endif

struct oledata {
    IDispatch *pDispatch;
};


struct oleeventdata {
    DWORD dwCookie;
    IConnectionPoint *pConnectionPoint;
    long event_id;
};

struct oleparam {
    DISPPARAMS dp;
    OLECHAR** pNamedArgs;
};

struct olerecorddata {
    IRecordInfo *pri;
    void *pdata;
};

static HRESULT ( STDMETHODCALLTYPE QueryInterface )(IDispatch __RPC_FAR *, REFIID riid, void __RPC_FAR *__RPC_FAR *ppvObject);
static ULONG ( STDMETHODCALLTYPE AddRef )(IDispatch __RPC_FAR * This);
static ULONG ( STDMETHODCALLTYPE Release )(IDispatch __RPC_FAR * This);
static HRESULT ( STDMETHODCALLTYPE GetTypeInfoCount )(IDispatch __RPC_FAR * This, UINT __RPC_FAR *pctinfo);
static HRESULT ( STDMETHODCALLTYPE GetTypeInfo )(IDispatch __RPC_FAR * This, UINT iTInfo, LCID lcid, ITypeInfo __RPC_FAR *__RPC_FAR *ppTInfo);
static HRESULT ( STDMETHODCALLTYPE GetIDsOfNames )(IDispatch __RPC_FAR * This, REFIID riid, LPOLESTR __RPC_FAR *rgszNames, UINT cNames, LCID lcid, DISPID __RPC_FAR *rgDispId);
static HRESULT ( STDMETHODCALLTYPE Invoke )( IDispatch __RPC_FAR * This, DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS __RPC_FAR *pDispParams, VARIANT __RPC_FAR *pVarResult, EXCEPINFO __RPC_FAR *pExcepInfo, UINT __RPC_FAR *puArgErr);
static IDispatch* val2dispatch(VALUE val);
static double rbtime2vtdate(VALUE tmobj);
static VALUE vtdate2rbtime(double date);
static rb_encoding *ole_cp2encoding(UINT cp);
static UINT ole_encoding2cp(rb_encoding *enc);
NORETURN(static void failed_load_conv51932(void));
#ifndef pIMultiLanguage
static void load_conv_function51932(void);
#endif
static UINT ole_init_cp(void);
static char *ole_wc2mb(LPWSTR pw);
static void ole_freeexceptinfo(EXCEPINFO *pExInfo);
static VALUE ole_excepinfo2msg(EXCEPINFO *pExInfo);
static void ole_msg_loop(void);
static void ole_free(struct oledata *pole);
static LPWSTR ole_mb2wc(char *pm, int len);
static VALUE ole_ary_m_entry(VALUE val, LONG *pid);
static VALUE is_all_index_under(LONG *pid, long *pub, long dim);
static void * get_ptr_of_variant(VARIANT *pvar);
static void ole_set_safe_array(long n, SAFEARRAY *psa, LONG *pid, long *pub, VALUE val, long dim,  VARTYPE vt);
static long dimension(VALUE val);
static long ary_len_of_dim(VALUE ary, long dim);
static int hash2olerec(VALUE key, VALUE val, VALUE rec);
static void ole_rec2variant(VALUE rec, VARIANT *var);
static void ole_val2ptr_variant(VALUE val, VARIANT *var);
static VALUE ole_set_member(VALUE self, IDispatch *dispatch);
static VALUE fole_s_allocate(VALUE klass);
static VALUE create_win32ole_object(VALUE klass, IDispatch *pDispatch, int argc, VALUE *argv);
static VALUE ary_new_dim(VALUE myary, LONG *pid, LONG *plb, LONG dim);
static void ary_store_dim(VALUE myary, LONG *pid, LONG *plb, LONG dim, VALUE val);
static void ole_const_load(ITypeLib *pTypeLib, VALUE klass, VALUE self);
static HRESULT clsid_from_remote(VALUE host, VALUE com, CLSID *pclsid);
static VALUE ole_create_dcom(VALUE self, VALUE ole, VALUE host, VALUE others);
static VALUE ole_bind_obj(VALUE moniker, int argc, VALUE *argv, VALUE self);
static VALUE fole_s_connect(int argc, VALUE *argv, VALUE self);
static VALUE fole_s_const_load(int argc, VALUE *argv, VALUE self);
static ULONG reference_count(struct oledata * pole);
static VALUE fole_s_reference_count(VALUE self, VALUE obj);
static VALUE fole_s_free(VALUE self, VALUE obj);
static HWND ole_show_help(VALUE helpfile, VALUE helpcontext);
static VALUE fole_s_show_help(int argc, VALUE *argv, VALUE self);
static VALUE fole_s_get_code_page(VALUE self);
static BOOL CALLBACK installed_code_page_proc(LPTSTR str);
static BOOL code_page_installed(UINT cp);
static VALUE fole_s_set_code_page(VALUE self, VALUE vcp);
static VALUE fole_s_get_locale(VALUE self);
static BOOL CALLBACK installed_lcid_proc(LPTSTR str);
static BOOL lcid_installed(LCID lcid);
static VALUE fole_s_set_locale(VALUE self, VALUE vlcid);
static VALUE fole_s_create_guid(VALUE self);
static VALUE fole_s_ole_initialize(VALUE self);
static VALUE fole_s_ole_uninitialize(VALUE self);
static VALUE fole_initialize(int argc, VALUE *argv, VALUE self);
static int hash2named_arg(VALUE key, VALUE val, VALUE pop);
static VALUE set_argv(VARIANTARG* realargs, unsigned int beg, unsigned int end);
static VALUE ole_invoke(int argc, VALUE *argv, VALUE self, USHORT wFlags, BOOL is_bracket);
static VALUE fole_invoke(int argc, VALUE *argv, VALUE self);
static VALUE ole_invoke2(VALUE self, VALUE dispid, VALUE args, VALUE types, USHORT dispkind);
static VALUE fole_invoke2(VALUE self, VALUE dispid, VALUE args, VALUE types);
static VALUE fole_getproperty2(VALUE self, VALUE dispid, VALUE args, VALUE types);
static VALUE fole_setproperty2(VALUE self, VALUE dispid, VALUE args, VALUE types);
static VALUE fole_setproperty_with_bracket(int argc, VALUE *argv, VALUE self);
static VALUE fole_setproperty(int argc, VALUE *argv, VALUE self);
static VALUE fole_getproperty_with_bracket(int argc, VALUE *argv, VALUE self);
static VALUE ole_propertyput(VALUE self, VALUE property, VALUE value);
static VALUE fole_free(VALUE self);
static VALUE ole_each_sub(VALUE pEnumV);
static VALUE ole_ienum_free(VALUE pEnumV);
static VALUE fole_each(VALUE self);
static VALUE fole_missing(int argc, VALUE *argv, VALUE self);
static HRESULT typeinfo_from_ole(struct oledata *pole, ITypeInfo **ppti);
static VALUE ole_methods(VALUE self, int mask);
static VALUE fole_methods(VALUE self);
static VALUE fole_get_methods(VALUE self);
static VALUE fole_put_methods(VALUE self);
static VALUE fole_func_methods(VALUE self);
static VALUE fole_type(VALUE self);
static VALUE fole_typelib(VALUE self);
static VALUE fole_query_interface(VALUE self, VALUE str_iid);
static VALUE fole_respond_to(VALUE self, VALUE method);
static VALUE ole_usertype2val(ITypeInfo *pTypeInfo, TYPEDESC *pTypeDesc, VALUE typedetails);
static VALUE ole_ptrtype2val(ITypeInfo *pTypeInfo, TYPEDESC *pTypeDesc, VALUE typedetails);
static VALUE fole_method_help(VALUE self, VALUE cmdname);
static VALUE fole_activex_initialize(VALUE self);


static long ole_search_event_at(VALUE ary, VALUE ev);
static VALUE ole_search_event(VALUE ary, VALUE ev, BOOL  *is_default);
static VALUE ole_search_handler_method(VALUE handler, VALUE ev, BOOL *is_default_handler);
static void ole_delete_event(VALUE ary, VALUE ev);
static void hash2ptr_dispparams(VALUE hash, ITypeInfo *pTypeInfo, DISPID dispid, DISPPARAMS *pdispparams);
static VALUE hash2result(VALUE hash);
static void ary2ptr_dispparams(VALUE ary, DISPPARAMS *pdispparams);
static VALUE exec_callback(VALUE arg);
static VALUE rescue_callback(VALUE arg);
static HRESULT find_iid(VALUE ole, char *pitf, IID *piid, ITypeInfo **ppTypeInfo);
static HRESULT find_coclass(ITypeInfo *pTypeInfo, TYPEATTR *pTypeAttr, ITypeInfo **pTypeInfo2, TYPEATTR **pTypeAttr2);
static HRESULT find_default_source_from_typeinfo(ITypeInfo *pTypeInfo, TYPEATTR *pTypeAttr, ITypeInfo **ppTypeInfo);
static HRESULT find_default_source(VALUE ole, IID *piid, ITypeInfo **ppTypeInfo);
static void ole_event_free(struct oleeventdata *poleev);
static VALUE fev_s_allocate(VALUE klass);
static VALUE ev_advise(int argc, VALUE *argv, VALUE self);
static VALUE fev_initialize(int argc, VALUE *argv, VALUE self);
static VALUE fev_s_msg_loop(VALUE klass);
static void add_event_call_back(VALUE obj, VALUE event, VALUE data);
static VALUE ev_on_event(int argc, VALUE *argv, VALUE self, VALUE is_ary_arg);
static VALUE fev_on_event(int argc, VALUE *argv, VALUE self);
static VALUE fev_on_event_with_outargs(int argc, VALUE *argv, VALUE self);
static VALUE fev_off_event(int argc, VALUE *argv, VALUE self);
static VALUE fev_unadvise(VALUE self);
static VALUE fev_set_handler(VALUE self, VALUE val);
static VALUE fev_get_handler(VALUE self);
static VALUE evs_push(VALUE ev);
static VALUE evs_delete(long i);
static VALUE evs_entry(long i);
static VALUE evs_length(void);

static HRESULT typelib_from_val(VALUE obj, ITypeLib **pTypeLib);
static HRESULT recordinfo_from_itypelib(ITypeLib *pTypeLib, VALUE name, IRecordInfo **ppri);
static void olerecord_set_ivar(VALUE obj, IRecordInfo *pri, void *prec);
static void olerecord_free(struct olerecorddata *pvar);
static VALUE folerecord_s_allocate(VALUE klass);
static VALUE folerecord_initialize(VALUE self, VALUE typename, VALUE oleobj);
static VALUE folerecord_to_h(VALUE self);
static VALUE folerecord_typename(VALUE self);
static VALUE olerecord_ivar_get(VALUE self, VALUE name);
static VALUE olerecord_ivar_set(VALUE self, VALUE name, VALUE val);
static VALUE folerecord_method_missing(int argc, VALUE *argv, VALUE self);
static VALUE folerecord_ole_instance_variable_get(VALUE self, VALUE name);
static VALUE folerecord_ole_instance_variable_set(VALUE self, VALUE name, VALUE val);
static VALUE folerecord_inspect(VALUE self);
static void init_enc2cp(void);
static void free_enc2cp(void);

static HRESULT (STDMETHODCALLTYPE mf_QueryInterface)(
    IMessageFilter __RPC_FAR * This,
    /* [in] */ REFIID riid,
    /* [iid_is][out] */ void __RPC_FAR *__RPC_FAR *ppvObject)
{
    if (MEMCMP(riid, &IID_IUnknown, GUID, 1) == 0
        || MEMCMP(riid, &IID_IMessageFilter, GUID, 1) == 0)
    {
        *ppvObject = &message_filter;
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG (STDMETHODCALLTYPE mf_AddRef)(
    IMessageFilter __RPC_FAR * This)
{
    return 1;
}

static ULONG (STDMETHODCALLTYPE mf_Release)(
    IMessageFilter __RPC_FAR * This)
{
    return 1;
}

static DWORD (STDMETHODCALLTYPE mf_HandleInComingCall)(
    IMessageFilter __RPC_FAR * pThis,
    DWORD dwCallType,      //Type of incoming call
    HTASK threadIDCaller,  //Task handle calling this task
    DWORD dwTickCount,     //Elapsed tick count
    LPINTERFACEINFO lpInterfaceInfo //Pointer to INTERFACEINFO structure
    )
{
#ifdef DEBUG_MESSAGEFILTER
    printf("incoming %08X, %08X, %d\n", dwCallType, threadIDCaller, dwTickCount);
    fflush(stdout);
#endif
    switch (dwCallType)
    {
    case CALLTYPE_ASYNC:
    case CALLTYPE_TOPLEVEL_CALLPENDING:
    case CALLTYPE_ASYNC_CALLPENDING:
        if (rb_during_gc()) {
            return SERVERCALL_RETRYLATER;
        }
        break;
    default:
        break;
    }
    if (previous_filter) {
        return previous_filter->lpVtbl->HandleInComingCall(previous_filter,
                                                   dwCallType,
                                                   threadIDCaller,
                                                   dwTickCount,
                                                   lpInterfaceInfo);
    }
    return SERVERCALL_ISHANDLED;
}

static DWORD (STDMETHODCALLTYPE mf_RetryRejectedCall)(
    IMessageFilter* pThis,
    HTASK threadIDCallee,  //Server task handle
    DWORD dwTickCount,     //Elapsed tick count
    DWORD dwRejectType     //Returned rejection message
    )
{
    if (previous_filter) {
        return previous_filter->lpVtbl->RetryRejectedCall(previous_filter,
                                                  threadIDCallee,
                                                  dwTickCount,
                                                  dwRejectType);
    }
    return 1000;
}

static DWORD (STDMETHODCALLTYPE mf_MessagePending)(
    IMessageFilter* pThis,
    HTASK threadIDCallee,  //Called applications task handle
    DWORD dwTickCount,     //Elapsed tick count
    DWORD dwPendingType    //Call type
    )
{
    if (rb_during_gc()) {
        return PENDINGMSG_WAITNOPROCESS;
    }
    if (previous_filter) {
        return previous_filter->lpVtbl->MessagePending(previous_filter,
                                               threadIDCallee,
                                               dwTickCount,
                                               dwPendingType);
    }
    return PENDINGMSG_WAITNOPROCESS;
}

typedef struct _Win32OLEIDispatch
{
    IDispatch dispatch;
    ULONG refcount;
    VALUE obj;
} Win32OLEIDispatch;

static HRESULT ( STDMETHODCALLTYPE QueryInterface )(
    IDispatch __RPC_FAR * This,
    /* [in] */ REFIID riid,
    /* [iid_is][out] */ void __RPC_FAR *__RPC_FAR *ppvObject)
{
    if (MEMCMP(riid, &IID_IUnknown, GUID, 1) == 0
        || MEMCMP(riid, &IID_IDispatch, GUID, 1) == 0)
    {
        Win32OLEIDispatch* p = (Win32OLEIDispatch*)This;
        p->refcount++;
        *ppvObject = This;
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG ( STDMETHODCALLTYPE AddRef )(
    IDispatch __RPC_FAR * This)
{
    Win32OLEIDispatch* p = (Win32OLEIDispatch*)This;
    return ++(p->refcount);
}

static ULONG ( STDMETHODCALLTYPE Release )(
    IDispatch __RPC_FAR * This)
{
    Win32OLEIDispatch* p = (Win32OLEIDispatch*)This;
    ULONG u = --(p->refcount);
    if (u == 0) {
        st_data_t key = p->obj;
        st_delete(DATA_PTR(com_hash), &key, 0);
        free(p);
    }
    return u;
}

static HRESULT ( STDMETHODCALLTYPE GetTypeInfoCount )(
    IDispatch __RPC_FAR * This,
    /* [out] */ UINT __RPC_FAR *pctinfo)
{
    return E_NOTIMPL;
}

static HRESULT ( STDMETHODCALLTYPE GetTypeInfo )(
    IDispatch __RPC_FAR * This,
    /* [in] */ UINT iTInfo,
    /* [in] */ LCID lcid,
    /* [out] */ ITypeInfo __RPC_FAR *__RPC_FAR *ppTInfo)
{
    return E_NOTIMPL;
}


static HRESULT ( STDMETHODCALLTYPE GetIDsOfNames )(
    IDispatch __RPC_FAR * This,
    /* [in] */ REFIID riid,
    /* [size_is][in] */ LPOLESTR __RPC_FAR *rgszNames,
    /* [in] */ UINT cNames,
    /* [in] */ LCID lcid,
    /* [size_is][out] */ DISPID __RPC_FAR *rgDispId)
{
    /*
    Win32OLEIDispatch* p = (Win32OLEIDispatch*)This;
    */
    char* psz = ole_wc2mb(*rgszNames); // support only one method
    ID nameid = rb_intern(psz);
    free(psz);
    if ((ID)(DISPID)nameid != nameid) return E_NOINTERFACE;
    *rgDispId = (DISPID)nameid;
    return S_OK;
}

static /* [local] */ HRESULT ( STDMETHODCALLTYPE Invoke )(
    IDispatch __RPC_FAR * This,
    /* [in] */ DISPID dispIdMember,
    /* [in] */ REFIID riid,
    /* [in] */ LCID lcid,
    /* [in] */ WORD wFlags,
    /* [out][in] */ DISPPARAMS __RPC_FAR *pDispParams,
    /* [out] */ VARIANT __RPC_FAR *pVarResult,
    /* [out] */ EXCEPINFO __RPC_FAR *pExcepInfo,
    /* [out] */ UINT __RPC_FAR *puArgErr)
{
    VALUE v;
    int i;
    int args = pDispParams->cArgs;
    Win32OLEIDispatch* p = (Win32OLEIDispatch*)This;
    VALUE* parg = ALLOCA_N(VALUE, args);
    ID mid = (ID)dispIdMember;
    for (i = 0; i < args; i++) {
        *(parg + i) = ole_variant2val(&pDispParams->rgvarg[args - i - 1]);
    }
    if (dispIdMember == DISPID_VALUE) {
        if (wFlags == DISPATCH_METHOD) {
            mid = rb_intern("call");
        } else if (wFlags & DISPATCH_PROPERTYGET) {
            mid = rb_intern("value");
        }
    }
    v = rb_funcall2(p->obj, mid, args, parg);
    ole_val2variant(v, pVarResult);
    return S_OK;
}

BOOL
ole_initialized()
{
    return g_ole_initialized;
}

static IDispatch*
val2dispatch(VALUE val)
{
    struct st_table *tbl = DATA_PTR(com_hash);
    Win32OLEIDispatch* pdisp;
    st_data_t data;

    if (st_lookup(tbl, val, &data)) {
        pdisp = (Win32OLEIDispatch *)(data & ~FIXNUM_FLAG);
        pdisp->refcount++;
    }
    else {
        pdisp = ALLOC(Win32OLEIDispatch);
        pdisp->dispatch.lpVtbl = &com_vtbl;
        pdisp->refcount = 1;
        pdisp->obj = val;
        st_insert(tbl, val, (VALUE)pdisp | FIXNUM_FLAG);
    }
    return &pdisp->dispatch;
}

static double
rbtime2vtdate(VALUE tmobj)
{
    SYSTEMTIME st;
    double t = 0;
    memset(&st, 0, sizeof(SYSTEMTIME));
    st.wYear = FIX2INT(rb_funcall(tmobj, rb_intern("year"), 0));
    st.wMonth = FIX2INT(rb_funcall(tmobj, rb_intern("month"), 0));
    st.wDay = FIX2INT(rb_funcall(tmobj, rb_intern("mday"), 0));
    st.wHour = FIX2INT(rb_funcall(tmobj, rb_intern("hour"), 0));
    st.wMinute = FIX2INT(rb_funcall(tmobj, rb_intern("min"), 0));
    st.wSecond = FIX2INT(rb_funcall(tmobj, rb_intern("sec"), 0));
    st.wMilliseconds = FIX2INT(rb_funcall(tmobj, rb_intern("nsec"), 0)) / 1000000;
    SystemTimeToVariantTime(&st, &t);
    return t;
}

static VALUE
vtdate2rbtime(double date)
{
    SYSTEMTIME st;
    VALUE v;
    VariantTimeToSystemTime(date, &st);

    v = rb_funcall(rb_cTime, rb_intern("new"), 6,
		      INT2FIX(st.wYear),
		      INT2FIX(st.wMonth),
		      INT2FIX(st.wDay),
		      INT2FIX(st.wHour),
		      INT2FIX(st.wMinute),
		      INT2FIX(st.wSecond));
    if (st.wMilliseconds > 0) {
	return rb_funcall(v, rb_intern("+"), 1, rb_float_new((double)(st.wMilliseconds / 1000.0)));
    }
    return v;
}

#define ENC_MACHING_CP(enc,encname,cp) if(strcasecmp(rb_enc_name((enc)),(encname)) == 0) return cp

static UINT ole_encoding2cp(rb_encoding *enc)
{
    /*
     * Is there any better solution to convert
     * Ruby encoding to Windows codepage???
     */
    ENC_MACHING_CP(enc, "Big5", 950);
    ENC_MACHING_CP(enc, "CP51932", 51932);
    ENC_MACHING_CP(enc, "CP850", 850);
    ENC_MACHING_CP(enc, "CP852", 852);
    ENC_MACHING_CP(enc, "CP855", 855);
    ENC_MACHING_CP(enc, "CP949", 949);
    ENC_MACHING_CP(enc, "EUC-JP", 20932);
    ENC_MACHING_CP(enc, "EUC-KR", 51949);
    ENC_MACHING_CP(enc, "EUC-TW", 51950);
    ENC_MACHING_CP(enc, "GB18030", 54936);
    ENC_MACHING_CP(enc, "GB2312", 20936);
    ENC_MACHING_CP(enc, "GBK", 936);
    ENC_MACHING_CP(enc, "IBM437", 437);
    ENC_MACHING_CP(enc, "IBM737", 737);
    ENC_MACHING_CP(enc, "IBM775", 775);
    ENC_MACHING_CP(enc, "IBM852", 852);
    ENC_MACHING_CP(enc, "IBM855", 855);
    ENC_MACHING_CP(enc, "IBM857", 857);
    ENC_MACHING_CP(enc, "IBM860", 860);
    ENC_MACHING_CP(enc, "IBM861", 861);
    ENC_MACHING_CP(enc, "IBM862", 862);
    ENC_MACHING_CP(enc, "IBM863", 863);
    ENC_MACHING_CP(enc, "IBM864", 864);
    ENC_MACHING_CP(enc, "IBM865", 865);
    ENC_MACHING_CP(enc, "IBM866", 866);
    ENC_MACHING_CP(enc, "IBM869", 869);
    ENC_MACHING_CP(enc, "ISO-2022-JP", 50220);
    ENC_MACHING_CP(enc, "ISO-8859-1", 28591);
    ENC_MACHING_CP(enc, "ISO-8859-15", 28605);
    ENC_MACHING_CP(enc, "ISO-8859-2", 28592);
    ENC_MACHING_CP(enc, "ISO-8859-3", 28593);
    ENC_MACHING_CP(enc, "ISO-8859-4", 28594);
    ENC_MACHING_CP(enc, "ISO-8859-5", 28595);
    ENC_MACHING_CP(enc, "ISO-8859-6", 28596);
    ENC_MACHING_CP(enc, "ISO-8859-7", 28597);
    ENC_MACHING_CP(enc, "ISO-8859-8", 28598);
    ENC_MACHING_CP(enc, "ISO-8859-9", 28599);
    ENC_MACHING_CP(enc, "KOI8-R", 20866);
    ENC_MACHING_CP(enc, "KOI8-U", 21866);
    ENC_MACHING_CP(enc, "Shift_JIS", 932);
    ENC_MACHING_CP(enc, "UTF-16BE", 1201);
    ENC_MACHING_CP(enc, "UTF-16LE", 1200);
    ENC_MACHING_CP(enc, "UTF-7", 65000);
    ENC_MACHING_CP(enc, "UTF-8", 65001);
    ENC_MACHING_CP(enc, "Windows-1250", 1250);
    ENC_MACHING_CP(enc, "Windows-1251", 1251);
    ENC_MACHING_CP(enc, "Windows-1252", 1252);
    ENC_MACHING_CP(enc, "Windows-1253", 1253);
    ENC_MACHING_CP(enc, "Windows-1254", 1254);
    ENC_MACHING_CP(enc, "Windows-1255", 1255);
    ENC_MACHING_CP(enc, "Windows-1256", 1256);
    ENC_MACHING_CP(enc, "Windows-1257", 1257);
    ENC_MACHING_CP(enc, "Windows-1258", 1258);
    ENC_MACHING_CP(enc, "Windows-31J", 932);
    ENC_MACHING_CP(enc, "Windows-874", 874);
    ENC_MACHING_CP(enc, "eucJP-ms", 20932);
    return CP_ACP;
}

static void
failed_load_conv51932(void)
{
    rb_raise(eWIN32OLERuntimeError, "fail to load convert function for CP51932");
}

#ifndef pIMultiLanguage
static void
load_conv_function51932(void)
{
    HRESULT hr = E_NOINTERFACE;
    void *p;
    if (!pIMultiLanguage) {
#if defined(HAVE_TYPE_IMULTILANGUAGE2)
	hr = CoCreateInstance(&CLSID_CMultiLanguage, NULL, CLSCTX_INPROC_SERVER,
		              &IID_IMultiLanguage2, &p);
#elif defined(HAVE_TYPE_IMULTILANGUAGE)
	hr = CoCreateInstance(&CLSID_CMultiLanguage, NULL, CLSCTX_INPROC_SERVER,
		              &IID_IMultiLanguage, &p);
#endif
	if (FAILED(hr)) {
	    failed_load_conv51932();
	}
	pIMultiLanguage = p;
    }
}
#else
#define load_conv_function51932() failed_load_conv51932()
#endif

#define conv_51932(cp) ((cp) == 51932 && (load_conv_function51932(), 1))

static void
set_ole_codepage(UINT cp)
{
    if (code_page_installed(cp)) {
        cWIN32OLE_cp = cp;
    } else {
        switch(cp) {
        case CP_ACP:
        case CP_OEMCP:
        case CP_MACCP:
        case CP_THREAD_ACP:
        case CP_SYMBOL:
        case CP_UTF7:
        case CP_UTF8:
            cWIN32OLE_cp = cp;
            break;
        case 51932:
            cWIN32OLE_cp = cp;
            load_conv_function51932();
            break;
        default:
            rb_raise(eWIN32OLERuntimeError, "codepage should be WIN32OLE::CP_ACP, WIN32OLE::CP_OEMCP, WIN32OLE::CP_MACCP, WIN32OLE::CP_THREAD_ACP, WIN32OLE::CP_SYMBOL, WIN32OLE::CP_UTF7, WIN32OLE::CP_UTF8, or installed codepage.");
            break;
        }
    }
    cWIN32OLE_enc = ole_cp2encoding(cWIN32OLE_cp);
}


static UINT
ole_init_cp(void)
{
    UINT cp;
    rb_encoding *encdef;
    encdef = rb_default_internal_encoding();
    if (!encdef) {
	encdef = rb_default_external_encoding();
    }
    cp = ole_encoding2cp(encdef);
    set_ole_codepage(cp);
    return cp;
}

struct myCPINFOEX {
  UINT MaxCharSize;
  BYTE DefaultChar[2];
  BYTE LeadByte[12];
  WCHAR UnicodeDefaultChar;
  UINT CodePage;
  char CodePageName[MAX_PATH];
};

static rb_encoding *
ole_cp2encoding(UINT cp)
{
    static BOOL (*pGetCPInfoEx)(UINT, DWORD, struct myCPINFOEX *) = NULL;
    struct myCPINFOEX* buf;
    VALUE enc_name;
    char *enc_cstr;
    int idx;

    if (!code_page_installed(cp)) {
	switch(cp) {
	  case CP_ACP:
	    cp = GetACP();
	    break;
	  case CP_OEMCP:
	    cp = GetOEMCP();
	    break;
	  case CP_MACCP:
	  case CP_THREAD_ACP:
	    if (!pGetCPInfoEx) {
		pGetCPInfoEx = (BOOL (*)(UINT, DWORD, struct myCPINFOEX *))
		    GetProcAddress(GetModuleHandle("kernel32"), "GetCPInfoEx");
		if (!pGetCPInfoEx) {
		    pGetCPInfoEx = (void*)-1;
		}
	    }
	    buf = ALLOCA_N(struct myCPINFOEX, 1);
	    ZeroMemory(buf, sizeof(struct myCPINFOEX));
	    if (pGetCPInfoEx == (void*)-1 || !pGetCPInfoEx(cp, 0, buf)) {
		rb_raise(eWIN32OLERuntimeError, "cannot map codepage to encoding.");
		break;	/* never reach here */
	    }
	    cp = buf->CodePage;
	    break;
	  case CP_SYMBOL:
	  case CP_UTF7:
	  case CP_UTF8:
	    break;
	  case 51932:
	    load_conv_function51932();
	    break;
	  default:
            rb_raise(eWIN32OLERuntimeError, "codepage should be WIN32OLE::CP_ACP, WIN32OLE::CP_OEMCP, WIN32OLE::CP_MACCP, WIN32OLE::CP_THREAD_ACP, WIN32OLE::CP_SYMBOL, WIN32OLE::CP_UTF7, WIN32OLE::CP_UTF8, or installed codepage.");
            break;
        }
    }

    enc_name = rb_sprintf("CP%d", cp);
    idx = rb_enc_find_index(enc_cstr = StringValueCStr(enc_name));
    if (idx < 0)
	idx = rb_define_dummy_encoding(enc_cstr);
    return rb_enc_from_index(idx);
}

static char *
ole_wc2mb_alloc(LPWSTR pw, char *(alloc)(UINT size, void *arg), void *arg)
{
    LPSTR pm;
    UINT size = 0;
    if (conv_51932(cWIN32OLE_cp)) {
#ifndef pIMultiLanguage
	DWORD dw = 0;
	HRESULT hr = pIMultiLanguage->lpVtbl->ConvertStringFromUnicode(pIMultiLanguage,
		&dw, cWIN32OLE_cp, pw, NULL, NULL, &size);
	if (FAILED(hr)) {
            ole_raise(hr, eWIN32OLERuntimeError, "fail to convert Unicode to CP%d", cWIN32OLE_cp);
	}
	pm = alloc(size, arg);
	hr = pIMultiLanguage->lpVtbl->ConvertStringFromUnicode(pIMultiLanguage,
		&dw, cWIN32OLE_cp, pw, NULL, pm, &size);
	if (FAILED(hr)) {
            ole_raise(hr, eWIN32OLERuntimeError, "fail to convert Unicode to CP%d", cWIN32OLE_cp);
	}
	pm[size] = '\0';
#endif
        return pm;
    }
    size = WideCharToMultiByte(cWIN32OLE_cp, 0, pw, -1, NULL, 0, NULL, NULL);
    if (size) {
        pm = alloc(size, arg);
        WideCharToMultiByte(cWIN32OLE_cp, 0, pw, -1, pm, size, NULL, NULL);
        pm[size] = '\0';
    }
    else {
        pm = alloc(0, arg);
        *pm = '\0';
    }
    return pm;
}

static char *
ole_alloc_str(UINT size, void *arg)
{
    return ALLOC_N(char, size + 1);
}

static char *
ole_wc2mb(LPWSTR pw)
{
    return ole_wc2mb_alloc(pw, ole_alloc_str, NULL);
}

static void
ole_freeexceptinfo(EXCEPINFO *pExInfo)
{
    SysFreeString(pExInfo->bstrDescription);
    SysFreeString(pExInfo->bstrSource);
    SysFreeString(pExInfo->bstrHelpFile);
}

static VALUE
ole_excepinfo2msg(EXCEPINFO *pExInfo)
{
    char error_code[40];
    char *pSource = NULL;
    char *pDescription = NULL;
    VALUE error_msg;
    if(pExInfo->pfnDeferredFillIn != NULL) {
        (*pExInfo->pfnDeferredFillIn)(pExInfo);
    }
    if (pExInfo->bstrSource != NULL) {
        pSource = ole_wc2mb(pExInfo->bstrSource);
    }
    if (pExInfo->bstrDescription != NULL) {
        pDescription = ole_wc2mb(pExInfo->bstrDescription);
    }
    if(pExInfo->wCode == 0) {
        sprintf(error_code, "\n    OLE error code:%lX in ", (unsigned long)pExInfo->scode);
    }
    else{
        sprintf(error_code, "\n    OLE error code:%u in ", pExInfo->wCode);
    }
    error_msg = rb_str_new2(error_code);
    if(pSource != NULL) {
        rb_str_cat2(error_msg, pSource);
    }
    else {
        rb_str_cat(error_msg, "<Unknown>", 9);
    }
    rb_str_cat2(error_msg, "\n      ");
    if(pDescription != NULL) {
        rb_str_cat2(error_msg, pDescription);
    }
    else {
        rb_str_cat2(error_msg, "<No Description>");
    }
    if(pSource) free(pSource);
    if(pDescription) free(pDescription);
    ole_freeexceptinfo(pExInfo);
    return error_msg;
}

void
ole_uninitialize(void)
{
    if (!g_ole_initialized) return;
    OleUninitialize();
    g_ole_initialized_set(FALSE);
}

static void
ole_uninitialize_hook(rb_event_flag_t evflag, VALUE data, VALUE self, ID mid, VALUE klass)
{
    ole_uninitialize();
}

void
ole_initialize(void)
{
    HRESULT hr;

    if(!g_uninitialize_hooked) {
	rb_add_event_hook(ole_uninitialize_hook, RUBY_EVENT_THREAD_END, Qnil);
	g_uninitialize_hooked = TRUE;
    }

    if(g_ole_initialized == FALSE) {
        hr = OleInitialize(NULL);
        if(FAILED(hr)) {
            ole_raise(hr, rb_eRuntimeError, "fail: OLE initialize");
        }
        g_ole_initialized_set(TRUE);

        hr = CoRegisterMessageFilter(&imessage_filter, &previous_filter);
        if(FAILED(hr)) {
            previous_filter = NULL;
            ole_raise(hr, rb_eRuntimeError, "fail: install OLE MessageFilter");
        }
    }
}

static void
ole_msg_loop() {
    MSG msg;
    while(PeekMessage(&msg,NULL,0,0,PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

static void
ole_free(struct oledata *pole)
{
    OLE_FREE(pole->pDispatch);
    free(pole);
}

LPWSTR
ole_vstr2wc(VALUE vstr)
{
    rb_encoding *enc;
    int cp;
    UINT size = 0;
    LPWSTR pw;
    st_data_t data;
    enc = rb_enc_get(vstr);

    if (st_lookup(enc2cp_table, (st_data_t)enc, &data)) {
        cp = (int)data;
    } else {
        cp = ole_encoding2cp(enc);
        if (code_page_installed(cp) ||
            cp == CP_ACP ||
            cp == CP_OEMCP ||
            cp == CP_MACCP ||
            cp == CP_THREAD_ACP ||
            cp == CP_SYMBOL ||
            cp == CP_UTF7 ||
            cp == CP_UTF8 ||
            cp == 51932) {
            st_insert(enc2cp_table, (st_data_t)enc, (st_data_t)cp);
        } else {
            rb_raise(eWIN32OLERuntimeError, "not installed Windows codepage(%d) according to `%s'", cp, rb_enc_name(enc));
        }
    }
    if (conv_51932(cp)) {
#ifndef pIMultiLanguage
	DWORD dw = 0;
	UINT len = RSTRING_LENINT(vstr);
	HRESULT hr = pIMultiLanguage->lpVtbl->ConvertStringToUnicode(pIMultiLanguage,
		&dw, cp, RSTRING_PTR(vstr), &len, NULL, &size);
	if (FAILED(hr)) {
            ole_raise(hr, eWIN32OLERuntimeError, "fail to convert CP%d to Unicode", cp);
	}
	pw = SysAllocStringLen(NULL, size);
	len = RSTRING_LEN(vstr);
	hr = pIMultiLanguage->lpVtbl->ConvertStringToUnicode(pIMultiLanguage,
		&dw, cp, RSTRING_PTR(vstr), &len, pw, &size);
	if (FAILED(hr)) {
            ole_raise(hr, eWIN32OLERuntimeError, "fail to convert CP%d to Unicode", cp);
	}
#endif
	return pw;
    }
    size = MultiByteToWideChar(cp, 0, RSTRING_PTR(vstr), RSTRING_LEN(vstr), NULL, 0);
    pw = SysAllocStringLen(NULL, size);
    MultiByteToWideChar(cp, 0, RSTRING_PTR(vstr), RSTRING_LEN(vstr), pw, size);
    return pw;
}

static LPWSTR
ole_mb2wc(char *pm, int len)
{
    UINT size = 0;
    LPWSTR pw;

    if (conv_51932(cWIN32OLE_cp)) {
#ifndef pIMultiLanguage
	DWORD dw = 0;
	UINT n = len;
	HRESULT hr = pIMultiLanguage->lpVtbl->ConvertStringToUnicode(pIMultiLanguage,
		&dw, cWIN32OLE_cp, pm, &n, NULL, &size);
	if (FAILED(hr)) {
            ole_raise(hr, eWIN32OLERuntimeError, "fail to convert CP%d to Unicode", cWIN32OLE_cp);
	}
	pw = SysAllocStringLen(NULL, size);
	hr = pIMultiLanguage->lpVtbl->ConvertStringToUnicode(pIMultiLanguage,
		&dw, cWIN32OLE_cp, pm, &n, pw, &size);
	if (FAILED(hr)) {
            ole_raise(hr, eWIN32OLERuntimeError, "fail to convert CP%d to Unicode", cWIN32OLE_cp);
	}
#endif
	return pw;
    }
    size = MultiByteToWideChar(cWIN32OLE_cp, 0, pm, len, NULL, 0);
    pw = SysAllocStringLen(NULL, size - 1);
    MultiByteToWideChar(cWIN32OLE_cp, 0, pm, len, pw, size);
    return pw;
}

static char *
ole_alloc_vstr(UINT size, void *arg)
{
    VALUE str = rb_enc_str_new(NULL, size, cWIN32OLE_enc);
    *(VALUE *)arg = str;
    return RSTRING_PTR(str);
}

VALUE
ole_wc2vstr(LPWSTR pw, BOOL isfree)
{
    VALUE vstr;
    ole_wc2mb_alloc(pw, ole_alloc_vstr, &vstr);
    rb_str_set_len(vstr, (long)strlen(RSTRING_PTR(vstr)));
    if(isfree)
        SysFreeString(pw);
    return vstr;
}

static VALUE
ole_ary_m_entry(VALUE val, LONG *pid)
{
    VALUE obj = Qnil;
    int i = 0;
    obj = val;
    while(RB_TYPE_P(obj, T_ARRAY)) {
        obj = rb_ary_entry(obj, pid[i]);
        i++;
    }
    return obj;
}

static VALUE
is_all_index_under(LONG *pid, long *pub, long dim)
{
  long i = 0;
  for (i = 0; i < dim; i++) {
    if (pid[i] > pub[i]) {
      return Qfalse;
    }
  }
  return Qtrue;
}

void
ole_val2variant_ex(VALUE val, VARIANT *var, VARTYPE vt)
{
    if (val == Qnil) {
        if (vt == VT_VARIANT) {
            ole_val2variant2(val, var);
        } else {
            V_VT(var) = (vt & ~VT_BYREF);
            if (V_VT(var) == VT_DISPATCH) {
                V_DISPATCH(var) = NULL;
            } else if (V_VT(var) == VT_UNKNOWN) {
                V_UNKNOWN(var) = NULL;
            }
        }
        return;
    }
#if (_MSC_VER >= 1300) || defined(__CYGWIN__) || defined(__MINGW32__)
    switch(vt & ~VT_BYREF) {
    case VT_I8:
        V_VT(var) = VT_I8;
        V_I8(var) = NUM2I8 (val);
        break;
    case VT_UI8:
        V_VT(var) = VT_UI8;
        V_UI8(var) = NUM2UI8(val);
        break;
    default:
        ole_val2variant2(val, var);
        break;
    }
#else  /* (_MSC_VER >= 1300) || defined(__CYGWIN__) || defined(__MINGW32__) */
    ole_val2variant2(val, var);
#endif
}

VOID *
val2variant_ptr(VALUE val, VARIANT *var, VARTYPE vt)
{
    VOID *p = NULL;
    HRESULT hr = S_OK;
    ole_val2variant_ex(val, var, vt);
    if ((vt & ~VT_BYREF) == VT_VARIANT) {
        p = var;
    } else {
        if ( (vt & ~VT_BYREF) != V_VT(var)) {
            hr = VariantChangeTypeEx(var, var,
                    cWIN32OLE_lcid, 0, (VARTYPE)(vt & ~VT_BYREF));
            if (FAILED(hr)) {
                ole_raise(hr, rb_eRuntimeError, "failed to change type");
            }
        }
        p = get_ptr_of_variant(var);
    }
    if (p == NULL) {
        rb_raise(rb_eRuntimeError, "failed to get pointer of variant");
    }
    return p;
}

static void *
get_ptr_of_variant(VARIANT *pvar)
{
    switch(V_VT(pvar)) {
    case VT_UI1:
        return &V_UI1(pvar);
        break;
    case VT_I2:
        return &V_I2(pvar);
        break;
    case VT_UI2:
        return &V_UI2(pvar);
        break;
    case VT_I4:
        return &V_I4(pvar);
        break;
    case VT_UI4:
        return &V_UI4(pvar);
        break;
    case VT_R4:
        return &V_R4(pvar);
        break;
    case VT_R8:
        return &V_R8(pvar);
        break;
#if (_MSC_VER >= 1300) || defined(__CYGWIN__) || defined(__MINGW32__)
    case VT_I8:
        return &V_I8(pvar);
        break;
    case VT_UI8:
        return &V_UI8(pvar);
        break;
#endif
    case VT_INT:
        return &V_INT(pvar);
        break;
    case VT_UINT:
        return &V_UINT(pvar);
        break;
    case VT_CY:
        return &V_CY(pvar);
        break;
    case VT_DATE:
        return &V_DATE(pvar);
        break;
    case VT_BSTR:
        return V_BSTR(pvar);
        break;
    case VT_DISPATCH:
        return V_DISPATCH(pvar);
        break;
    case VT_ERROR:
        return &V_ERROR(pvar);
        break;
    case VT_BOOL:
        return &V_BOOL(pvar);
        break;
    case VT_UNKNOWN:
        return V_UNKNOWN(pvar);
        break;
    case VT_ARRAY:
        return &V_ARRAY(pvar);
        break;
    default:
        return NULL;
        break;
    }
}

static void
ole_set_safe_array(long n, SAFEARRAY *psa, LONG *pid, long *pub, VALUE val, long dim,  VARTYPE vt)
{
    VALUE val1;
    HRESULT hr = S_OK;
    VARIANT var;
    VOID *p = NULL;
    long i = n;
    while(i >= 0) {
        val1 = ole_ary_m_entry(val, pid);
        VariantInit(&var);
        p = val2variant_ptr(val1, &var, vt);
        if (is_all_index_under(pid, pub, dim) == Qtrue) {
            if ((V_VT(&var) == VT_DISPATCH && V_DISPATCH(&var) == NULL) ||
                (V_VT(&var) == VT_UNKNOWN && V_UNKNOWN(&var) == NULL)) {
                rb_raise(eWIN32OLERuntimeError, "element of array does not have IDispatch or IUnknown Interface");
            }
            hr = SafeArrayPutElement(psa, pid, p);
        }
        if (FAILED(hr)) {
            ole_raise(hr, rb_eRuntimeError, "failed to SafeArrayPutElement");
        }
        pid[i] += 1;
        if (pid[i] > pub[i]) {
            pid[i] = 0;
            i -= 1;
        } else {
            i = dim - 1;
        }
    }
}

static long
dimension(VALUE val) {
    long dim = 0;
    long dim1 = 0;
    long len = 0;
    long i = 0;
    if (RB_TYPE_P(val, T_ARRAY)) {
        len = RARRAY_LEN(val);
        for (i = 0; i < len; i++) {
            dim1 = dimension(rb_ary_entry(val, i));
            if (dim < dim1) {
                dim = dim1;
            }
        }
        dim += 1;
    }
    return dim;
}

static long
ary_len_of_dim(VALUE ary, long dim) {
    long ary_len = 0;
    long ary_len1 = 0;
    long len = 0;
    long i = 0;
    VALUE val;
    if (dim == 0) {
        if (RB_TYPE_P(ary, T_ARRAY)) {
            ary_len = RARRAY_LEN(ary);
        }
    } else {
        if (RB_TYPE_P(ary, T_ARRAY)) {
            len = RARRAY_LEN(ary);
            for (i = 0; i < len; i++) {
                val = rb_ary_entry(ary, i);
                ary_len1 = ary_len_of_dim(val, dim-1);
                if (ary_len < ary_len1) {
                    ary_len = ary_len1;
                }
            }
        }
    }
    return ary_len;
}

HRESULT
ole_val_ary2variant_ary(VALUE val, VARIANT *var, VARTYPE vt)
{
    long dim = 0;
    int  i = 0;
    HRESULT hr = S_OK;

    SAFEARRAYBOUND *psab = NULL;
    SAFEARRAY *psa = NULL;
    long      *pub;
    LONG      *pid;

    Check_Type(val, T_ARRAY);

    dim = dimension(val);

    psab = ALLOC_N(SAFEARRAYBOUND, dim);
    pub  = ALLOC_N(long, dim);
    pid  = ALLOC_N(LONG, dim);

    if(!psab || !pub || !pid) {
        if(pub) free(pub);
        if(psab) free(psab);
        if(pid) free(pid);
        rb_raise(rb_eRuntimeError, "memory allocation error");
    }

    for (i = 0; i < dim; i++) {
        psab[i].cElements = ary_len_of_dim(val, i);
        psab[i].lLbound = 0;
        pub[i] = psab[i].cElements - 1;
        pid[i] = 0;
    }
    /* Create and fill VARIANT array */
    if ((vt & ~VT_BYREF) == VT_ARRAY) {
        vt = (vt | VT_VARIANT);
    }
    psa = SafeArrayCreate((VARTYPE)(vt & VT_TYPEMASK), dim, psab);
    if (psa == NULL)
        hr = E_OUTOFMEMORY;
    else
        hr = SafeArrayLock(psa);
    if (SUCCEEDED(hr)) {
        ole_set_safe_array(dim-1, psa, pid, pub, val, dim, (VARTYPE)(vt & VT_TYPEMASK));
        hr = SafeArrayUnlock(psa);
    }

    if(pub) free(pub);
    if(psab) free(psab);
    if(pid) free(pid);

    if (SUCCEEDED(hr)) {
        V_VT(var) = vt;
        V_ARRAY(var) = psa;
    }
    else {
        if (psa != NULL)
            SafeArrayDestroy(psa);
    }
    return hr;
}

static int
hash2olerec(VALUE key, VALUE val, VALUE rec)
{
    VARIANT var;
    OLECHAR *pbuf;
    struct olerecorddata *prec;
    IRecordInfo *pri;
    HRESULT hr;

    if (val != Qnil) {
        Data_Get_Struct(rec, struct olerecorddata, prec);
        pri = prec->pri;
        VariantInit(&var);
        ole_val2variant(val, &var);
        pbuf = ole_vstr2wc(key);
        hr = pri->lpVtbl->PutField(pri, INVOKE_PROPERTYPUT, prec->pdata, pbuf, &var);
        SysFreeString(pbuf);
        VariantClear(&var);
        if (FAILED(hr)) {
            ole_raise(hr, eWIN32OLERuntimeError, "failed to putfield of `%s`", StringValuePtr(key));
        }
    }
    return ST_CONTINUE;
}

static void
ole_rec2variant(VALUE rec, VARIANT *var)
{
    struct olerecorddata *prec;
    ULONG size = 0;
    IRecordInfo *pri;
    HRESULT hr;
    VALUE fields;
    Data_Get_Struct(rec, struct olerecorddata, prec);
    pri = prec->pri;
    if (pri) {
        hr = pri->lpVtbl->GetSize(pri, &size);
        if (FAILED(hr)) {
            ole_raise(hr, eWIN32OLERuntimeError, "failed to get size for allocation of VT_RECORD object");
        }
        if (prec->pdata) {
            free(prec->pdata);
        }
        prec->pdata = ALLOC_N(char, size);
        if (!prec->pdata) {
            rb_raise(rb_eRuntimeError, "failed to memory allocation of %lu bytes", (unsigned long)size);
        }
        hr = pri->lpVtbl->RecordInit(pri, prec->pdata);
        if (FAILED(hr)) {
            ole_raise(hr, eWIN32OLERuntimeError, "failed to initialize VT_RECORD object");
        }
        fields = folerecord_to_h(rec);
        rb_hash_foreach(fields, hash2olerec, rec);
        V_RECORDINFO(var) = pri;
        V_RECORD(var) = prec->pdata;
        V_VT(var) = VT_RECORD;
    } else {
        rb_raise(eWIN32OLERuntimeError, "failed to retrieve IRecordInfo interface");
    }
}

void
ole_val2variant(VALUE val, VARIANT *var)
{
    struct oledata *pole;
    struct olevariantdata *pvar;
    if(rb_obj_is_kind_of(val, cWIN32OLE)) {
        Data_Get_Struct(val, struct oledata, pole);
        OLE_ADDREF(pole->pDispatch);
        V_VT(var) = VT_DISPATCH;
        V_DISPATCH(var) = pole->pDispatch;
        return;
    }
    if (rb_obj_is_kind_of(val, cWIN32OLE_VARIANT)) {
        Data_Get_Struct(val, struct olevariantdata, pvar);
        VariantCopy(var, &(pvar->var));
        return;
    }
    if (rb_obj_is_kind_of(val, cWIN32OLE_RECORD)) {
        ole_rec2variant(val, var);
        return;
    }
    if (rb_obj_is_kind_of(val, rb_cTime)) {
        V_VT(var) = VT_DATE;
        V_DATE(var) = rbtime2vtdate(val);
        return;
    }
    switch (TYPE(val)) {
    case T_ARRAY:
        ole_val_ary2variant_ary(val, var, VT_VARIANT|VT_ARRAY);
        break;
    case T_STRING:
        V_VT(var) = VT_BSTR;
        V_BSTR(var) = ole_vstr2wc(val);
        break;
    case T_FIXNUM:
        V_VT(var) = VT_I4;
        V_I4(var) = NUM2INT(val);
        break;
    case T_BIGNUM:
        V_VT(var) = VT_R8;
        V_R8(var) = rb_big2dbl(val);
        break;
    case T_FLOAT:
        V_VT(var) = VT_R8;
        V_R8(var) = NUM2DBL(val);
        break;
    case T_TRUE:
        V_VT(var) = VT_BOOL;
        V_BOOL(var) = VARIANT_TRUE;
        break;
    case T_FALSE:
        V_VT(var) = VT_BOOL;
        V_BOOL(var) = VARIANT_FALSE;
        break;
    case T_NIL:
        if (g_nil_to == VT_ERROR) {
            V_VT(var) = VT_ERROR;
            V_ERROR(var) = DISP_E_PARAMNOTFOUND;
        }else {
            V_VT(var) = VT_EMPTY;
        }
        break;
    default:
        V_VT(var) = VT_DISPATCH;
        V_DISPATCH(var) = val2dispatch(val);
        break;
    }
}

static void
ole_val2ptr_variant(VALUE val, VARIANT *var)
{
    switch (TYPE(val)) {
    case T_STRING:
        if (V_VT(var) == (VT_BSTR | VT_BYREF)) {
            *V_BSTRREF(var) = ole_vstr2wc(val);
        }
        break;
    case T_FIXNUM:
        switch(V_VT(var)) {
        case (VT_UI1 | VT_BYREF) :
            *V_UI1REF(var) = NUM2CHR(val);
            break;
        case (VT_I2 | VT_BYREF) :
            *V_I2REF(var) = (short)NUM2INT(val);
            break;
        case (VT_I4 | VT_BYREF) :
            *V_I4REF(var) = NUM2INT(val);
            break;
        case (VT_R4 | VT_BYREF) :
            *V_R4REF(var) = (float)NUM2INT(val);
            break;
        case (VT_R8 | VT_BYREF) :
            *V_R8REF(var) = NUM2INT(val);
            break;
        default:
            break;
        }
        break;
    case T_FLOAT:
        switch(V_VT(var)) {
        case (VT_I2 | VT_BYREF) :
            *V_I2REF(var) = (short)NUM2INT(val);
            break;
        case (VT_I4 | VT_BYREF) :
            *V_I4REF(var) = NUM2INT(val);
            break;
        case (VT_R4 | VT_BYREF) :
            *V_R4REF(var) = (float)NUM2DBL(val);
            break;
        case (VT_R8 | VT_BYREF) :
            *V_R8REF(var) = NUM2DBL(val);
            break;
        default:
            break;
        }
        break;
    case T_BIGNUM:
        if (V_VT(var) == (VT_R8 | VT_BYREF)) {
            *V_R8REF(var) = rb_big2dbl(val);
        }
        break;
    case T_TRUE:
        if (V_VT(var) == (VT_BOOL | VT_BYREF)) {
            *V_BOOLREF(var) = VARIANT_TRUE;
        }
        break;
    case T_FALSE:
        if (V_VT(var) == (VT_BOOL | VT_BYREF)) {
            *V_BOOLREF(var) = VARIANT_FALSE;
        }
        break;
    default:
        break;
    }
}

void
ole_val2variant2(VALUE val, VARIANT *var)
{
    g_nil_to = VT_EMPTY;
    ole_val2variant(val, var);
    g_nil_to = VT_ERROR;
}

VALUE
make_inspect(const char *class_name, VALUE detail)
{
    VALUE str;
    str = rb_str_new2("#<");
    rb_str_cat2(str, class_name);
    rb_str_cat2(str, ":");
    rb_str_concat(str, detail);
    rb_str_cat2(str, ">");
    return str;
}

VALUE
default_inspect(VALUE self, const char *class_name)
{
    VALUE detail = rb_funcall(self, rb_intern("to_s"), 0);
    return make_inspect(class_name, detail);
}

static VALUE
ole_set_member(VALUE self, IDispatch *dispatch)
{
    struct oledata *pole;
    Data_Get_Struct(self, struct oledata, pole);
    if (pole->pDispatch) {
        OLE_RELEASE(pole->pDispatch);
        pole->pDispatch = NULL;
    }
    pole->pDispatch = dispatch;
    return self;
}


static VALUE
fole_s_allocate(VALUE klass)
{
    struct oledata *pole;
    VALUE obj;
    ole_initialize();
    obj = Data_Make_Struct(klass,struct oledata,0,ole_free,pole);
    pole->pDispatch = NULL;
    return obj;
}

static VALUE
create_win32ole_object(VALUE klass, IDispatch *pDispatch, int argc, VALUE *argv)
{
    VALUE obj = fole_s_allocate(klass);
    ole_set_member(obj, pDispatch);
    return obj;
}

static VALUE
ary_new_dim(VALUE myary, LONG *pid, LONG *plb, LONG dim) {
    long i;
    VALUE obj = Qnil;
    VALUE pobj = Qnil;
    long *ids = ALLOC_N(long, dim);
    if (!ids) {
        rb_raise(rb_eRuntimeError, "memory allocation error");
    }
    for(i = 0; i < dim; i++) {
        ids[i] = pid[i] - plb[i];
    }
    obj = myary;
    pobj = myary;
    for(i = 0; i < dim-1; i++) {
        obj = rb_ary_entry(pobj, ids[i]);
        if (obj == Qnil) {
            rb_ary_store(pobj, ids[i], rb_ary_new());
        }
        obj = rb_ary_entry(pobj, ids[i]);
        pobj = obj;
    }
    if (ids) free(ids);
    return obj;
}

static void
ary_store_dim(VALUE myary, LONG *pid, LONG *plb, LONG dim, VALUE val) {
    long id = pid[dim - 1] - plb[dim - 1];
    VALUE obj = ary_new_dim(myary, pid, plb, dim);
    rb_ary_store(obj, id, val);
}

VALUE
ole_variant2val(VARIANT *pvar)
{
    VALUE obj = Qnil;
    VARTYPE vt = V_VT(pvar);
    HRESULT hr;
    while ( vt == (VT_BYREF | VT_VARIANT) ) {
        pvar = V_VARIANTREF(pvar);
        vt = V_VT(pvar);
    }

    if(V_ISARRAY(pvar)) {
        VARTYPE vt_base = vt & VT_TYPEMASK;
        SAFEARRAY *psa = V_ISBYREF(pvar) ? *V_ARRAYREF(pvar) : V_ARRAY(pvar);
        UINT i = 0;
        LONG *pid, *plb, *pub;
        VARIANT variant;
        VALUE val;
        UINT dim = 0;
        if (!psa) {
            return obj;
        }
        dim = SafeArrayGetDim(psa);
        pid = ALLOC_N(LONG, dim);
        plb = ALLOC_N(LONG, dim);
        pub = ALLOC_N(LONG, dim);

        if(!pid || !plb || !pub) {
            if(pid) free(pid);
            if(plb) free(plb);
            if(pub) free(pub);
            rb_raise(rb_eRuntimeError, "memory allocation error");
        }

        for(i = 0; i < dim; ++i) {
            SafeArrayGetLBound(psa, i+1, &plb[i]);
            SafeArrayGetLBound(psa, i+1, &pid[i]);
            SafeArrayGetUBound(psa, i+1, &pub[i]);
        }
        hr = SafeArrayLock(psa);
        if (SUCCEEDED(hr)) {
            obj = rb_ary_new();
            i = 0;
            VariantInit(&variant);
            V_VT(&variant) = vt_base | VT_BYREF;
            if (vt_base == VT_RECORD) {
                hr = SafeArrayGetRecordInfo(psa, &V_RECORDINFO(&variant));
                if (SUCCEEDED(hr)) {
                    V_VT(&variant) = VT_RECORD;
                }
            }
            while (i < dim) {
                ary_new_dim(obj, pid, plb, dim);
                if (vt_base == VT_RECORD)
                    hr = SafeArrayPtrOfIndex(psa, pid, &V_RECORD(&variant));
                else
                    hr = SafeArrayPtrOfIndex(psa, pid, &V_BYREF(&variant));
                if (SUCCEEDED(hr)) {
                    val = ole_variant2val(&variant);
                    ary_store_dim(obj, pid, plb, dim, val);
                }
                for (i = 0; i < dim; ++i) {
                    if (++pid[i] <= pub[i])
                        break;
                    pid[i] = plb[i];
                }
            }
            SafeArrayUnlock(psa);
        }
        if(pid) free(pid);
        if(plb) free(plb);
        if(pub) free(pub);
        return obj;
    }
    switch(V_VT(pvar) & ~VT_BYREF){
    case VT_EMPTY:
        break;
    case VT_NULL:
        break;
    case VT_I1:
        if(V_ISBYREF(pvar))
            obj = INT2NUM((long)*V_I1REF(pvar));
        else
            obj = INT2NUM((long)V_I1(pvar));
        break;

    case VT_UI1:
        if(V_ISBYREF(pvar))
            obj = INT2NUM((long)*V_UI1REF(pvar));
        else
            obj = INT2NUM((long)V_UI1(pvar));
        break;

    case VT_I2:
        if(V_ISBYREF(pvar))
            obj = INT2NUM((long)*V_I2REF(pvar));
        else
            obj = INT2NUM((long)V_I2(pvar));
        break;

    case VT_UI2:
        if(V_ISBYREF(pvar))
            obj = INT2NUM((long)*V_UI2REF(pvar));
        else
            obj = INT2NUM((long)V_UI2(pvar));
        break;

    case VT_I4:
        if(V_ISBYREF(pvar))
            obj = INT2NUM((long)*V_I4REF(pvar));
        else
            obj = INT2NUM((long)V_I4(pvar));
        break;

    case VT_UI4:
        if(V_ISBYREF(pvar))
            obj = INT2NUM((long)*V_UI4REF(pvar));
        else
            obj = INT2NUM((long)V_UI4(pvar));
        break;

    case VT_INT:
        if(V_ISBYREF(pvar))
            obj = INT2NUM((long)*V_INTREF(pvar));
        else
            obj = INT2NUM((long)V_INT(pvar));
        break;

    case VT_UINT:
        if(V_ISBYREF(pvar))
            obj = INT2NUM((long)*V_UINTREF(pvar));
        else
            obj = INT2NUM((long)V_UINT(pvar));
        break;

#if (_MSC_VER >= 1300) || defined(__CYGWIN__) || defined(__MINGW32__)
    case VT_I8:
        if(V_ISBYREF(pvar))
#if (_MSC_VER >= 1300) || defined(__CYGWIN__) || defined(__MINGW32__)
#ifdef V_I8REF
            obj = I8_2_NUM(*V_I8REF(pvar));
#endif
#else
            obj = Qnil;
#endif
        else
            obj = I8_2_NUM(V_I8(pvar));
        break;
    case VT_UI8:
        if(V_ISBYREF(pvar))
#if (_MSC_VER >= 1300) || defined(__CYGWIN__) || defined(__MINGW32__)
#ifdef V_UI8REF
            obj = UI8_2_NUM(*V_UI8REF(pvar));
#endif
#else
            obj = Qnil;
#endif
        else
            obj = UI8_2_NUM(V_UI8(pvar));
        break;
#endif  /* (_MSC_VER >= 1300) || defined(__CYGWIN__) || defined(__MINGW32__) */

    case VT_R4:
        if(V_ISBYREF(pvar))
            obj = rb_float_new(*V_R4REF(pvar));
        else
            obj = rb_float_new(V_R4(pvar));
        break;

    case VT_R8:
        if(V_ISBYREF(pvar))
            obj = rb_float_new(*V_R8REF(pvar));
        else
            obj = rb_float_new(V_R8(pvar));
        break;

    case VT_BSTR:
    {
        if(V_ISBYREF(pvar))
            obj = ole_wc2vstr(*V_BSTRREF(pvar), FALSE);
        else
            obj = ole_wc2vstr(V_BSTR(pvar), FALSE);
        break;
    }

    case VT_ERROR:
        if(V_ISBYREF(pvar))
            obj = INT2NUM(*V_ERRORREF(pvar));
        else
            obj = INT2NUM(V_ERROR(pvar));
        break;

    case VT_BOOL:
        if (V_ISBYREF(pvar))
            obj = (*V_BOOLREF(pvar) ? Qtrue : Qfalse);
        else
            obj = (V_BOOL(pvar) ? Qtrue : Qfalse);
        break;

    case VT_DISPATCH:
    {
        IDispatch *pDispatch;

        if (V_ISBYREF(pvar))
            pDispatch = *V_DISPATCHREF(pvar);
        else
            pDispatch = V_DISPATCH(pvar);

        if (pDispatch != NULL ) {
            OLE_ADDREF(pDispatch);
            obj = create_win32ole_object(cWIN32OLE, pDispatch, 0, 0);
        }
        break;
    }

    case VT_UNKNOWN:
    {
        /* get IDispatch interface from IUnknown interface */
        IUnknown *punk;
        IDispatch *pDispatch;
        void *p;
        HRESULT hr;

        if (V_ISBYREF(pvar))
            punk = *V_UNKNOWNREF(pvar);
        else
            punk = V_UNKNOWN(pvar);

        if(punk != NULL) {
           hr = punk->lpVtbl->QueryInterface(punk, &IID_IDispatch, &p);
           if(SUCCEEDED(hr)) {
               pDispatch = p;
               obj = create_win32ole_object(cWIN32OLE, pDispatch, 0, 0);
           }
        }
        break;
    }

    case VT_DATE:
    {
        DATE date;
        if(V_ISBYREF(pvar))
            date = *V_DATEREF(pvar);
        else
            date = V_DATE(pvar);

        obj =  vtdate2rbtime(date);
        break;
    }

    case VT_RECORD:
    {
        IRecordInfo *pri = V_RECORDINFO(pvar);
        void *prec = V_RECORD(pvar);
        obj = folerecord_s_allocate(cWIN32OLE_RECORD);
        olerecord_set_ivar(obj, pri, prec);
        break;
    }

    case VT_CY:
    default:
        {
        HRESULT hr;
        VARIANT variant;
        VariantInit(&variant);
        hr = VariantChangeTypeEx(&variant, pvar,
                                  cWIN32OLE_lcid, 0, VT_BSTR);
        if (SUCCEEDED(hr) && V_VT(&variant) == VT_BSTR) {
            obj = ole_wc2vstr(V_BSTR(&variant), FALSE);
        }
        VariantClear(&variant);
        break;
        }
    }
    return obj;
}

LONG
reg_open_key(HKEY hkey, const char *name, HKEY *phkey)
{
    return RegOpenKeyEx(hkey, name, 0, KEY_READ, phkey);
}

LONG
reg_open_vkey(HKEY hkey, VALUE key, HKEY *phkey)
{
    return reg_open_key(hkey, StringValuePtr(key), phkey);
}

VALUE
reg_enum_key(HKEY hkey, DWORD i)
{
    char buf[BUFSIZ + 1];
    DWORD size_buf = sizeof(buf);
    FILETIME ft;
    LONG err = RegEnumKeyEx(hkey, i, buf, &size_buf,
                            NULL, NULL, NULL, &ft);
    if(err == ERROR_SUCCESS) {
        buf[BUFSIZ] = '\0';
        return rb_str_new2(buf);
    }
    return Qnil;
}

VALUE
reg_get_val(HKEY hkey, const char *subkey)
{
    char *pbuf;
    DWORD dwtype = 0;
    DWORD size = 0;
    VALUE val = Qnil;
    LONG err = RegQueryValueEx(hkey, subkey, NULL, &dwtype, NULL, &size);

    if (err == ERROR_SUCCESS) {
        pbuf = ALLOC_N(char, size + 1);
        err = RegQueryValueEx(hkey, subkey, NULL, &dwtype, (BYTE *)pbuf, &size);
        if (err == ERROR_SUCCESS) {
            pbuf[size] = '\0';
            if (dwtype == REG_EXPAND_SZ) {
		char* pbuf2 = (char *)pbuf;
		DWORD len = ExpandEnvironmentStrings(pbuf2, NULL, 0);
		pbuf = ALLOC_N(char, len + 1);
		ExpandEnvironmentStrings(pbuf2, pbuf, len + 1);
		free(pbuf2);
            }
            val = rb_str_new2((char *)pbuf);
        }
        free(pbuf);
    }
    return val;
}

VALUE
reg_get_val2(HKEY hkey, const char *subkey)
{
    HKEY hsubkey;
    LONG err;
    VALUE val = Qnil;
    err = RegOpenKeyEx(hkey, subkey, 0, KEY_READ, &hsubkey);
    if (err == ERROR_SUCCESS) {
        val = reg_get_val(hsubkey, NULL);
        RegCloseKey(hsubkey);
    }
    if (val == Qnil) {
        val = reg_get_val(hkey, subkey);
    }
    return val;
}

static void
ole_const_load(ITypeLib *pTypeLib, VALUE klass, VALUE self)
{
    unsigned int count;
    unsigned int index;
    int iVar;
    ITypeInfo *pTypeInfo;
    TYPEATTR  *pTypeAttr;
    VARDESC   *pVarDesc;
    HRESULT hr;
    unsigned int len;
    BSTR bstr;
    char *pName = NULL;
    VALUE val;
    VALUE constant;
    ID id;
    constant = rb_hash_new();
    count = pTypeLib->lpVtbl->GetTypeInfoCount(pTypeLib);
    for (index = 0; index < count; index++) {
        hr = pTypeLib->lpVtbl->GetTypeInfo(pTypeLib, index, &pTypeInfo);
        if (FAILED(hr))
            continue;
        hr = OLE_GET_TYPEATTR(pTypeInfo, &pTypeAttr);
        if(FAILED(hr)) {
            OLE_RELEASE(pTypeInfo);
            continue;
        }
        for(iVar = 0; iVar < pTypeAttr->cVars; iVar++) {
            hr = pTypeInfo->lpVtbl->GetVarDesc(pTypeInfo, iVar, &pVarDesc);
            if(FAILED(hr))
                continue;
            if(pVarDesc->varkind == VAR_CONST &&
               !(pVarDesc->wVarFlags & (VARFLAG_FHIDDEN |
                                        VARFLAG_FRESTRICTED |
                                        VARFLAG_FNONBROWSABLE))) {
                hr = pTypeInfo->lpVtbl->GetNames(pTypeInfo, pVarDesc->memid, &bstr,
                                                 1, &len);
                if(FAILED(hr) || len == 0 || !bstr)
                    continue;
                pName = ole_wc2mb(bstr);
                val = ole_variant2val(V_UNION1(pVarDesc, lpvarValue));
                *pName = toupper((int)*pName);
                id = rb_intern(pName);
                if (rb_is_const_id(id)) {
                    rb_define_const(klass, pName, val);
                }
                else {
                    rb_hash_aset(constant, rb_str_new2(pName), val);
                }
                SysFreeString(bstr);
                if(pName) {
                    free(pName);
                    pName = NULL;
                }
            }
            pTypeInfo->lpVtbl->ReleaseVarDesc(pTypeInfo, pVarDesc);
        }
        pTypeInfo->lpVtbl->ReleaseTypeAttr(pTypeInfo, pTypeAttr);
        OLE_RELEASE(pTypeInfo);
    }
    rb_define_const(klass, "CONSTANTS", constant);
}

static HRESULT
clsid_from_remote(VALUE host, VALUE com, CLSID *pclsid)
{
    HKEY hlm;
    HKEY hpid;
    VALUE subkey;
    LONG err;
    char clsid[100];
    OLECHAR *pbuf;
    DWORD len;
    DWORD dwtype;
    HRESULT hr = S_OK;
    err = RegConnectRegistry(StringValuePtr(host), HKEY_LOCAL_MACHINE, &hlm);
    if (err != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(err);
    subkey = rb_str_new2("SOFTWARE\\Classes\\");
    rb_str_concat(subkey, com);
    rb_str_cat2(subkey, "\\CLSID");
    err = RegOpenKeyEx(hlm, StringValuePtr(subkey), 0, KEY_READ, &hpid);
    if (err != ERROR_SUCCESS)
        hr = HRESULT_FROM_WIN32(err);
    else {
        len = sizeof(clsid);
        err = RegQueryValueEx(hpid, "", NULL, &dwtype, (BYTE *)clsid, &len);
        if (err == ERROR_SUCCESS && dwtype == REG_SZ) {
            pbuf  = ole_mb2wc(clsid, -1);
            hr = CLSIDFromString(pbuf, pclsid);
            SysFreeString(pbuf);
        }
        else {
            hr = HRESULT_FROM_WIN32(err);
        }
        RegCloseKey(hpid);
    }
    RegCloseKey(hlm);
    return hr;
}

static VALUE
ole_create_dcom(VALUE self, VALUE ole, VALUE host, VALUE others)
{
    HRESULT hr;
    CLSID   clsid;
    OLECHAR *pbuf;

    COSERVERINFO serverinfo;
    MULTI_QI multi_qi;
    DWORD clsctx = CLSCTX_REMOTE_SERVER;

    if (!gole32)
        gole32 = LoadLibrary("OLE32");
    if (!gole32)
        rb_raise(rb_eRuntimeError, "failed to load OLE32");
    if (!gCoCreateInstanceEx)
        gCoCreateInstanceEx = (FNCOCREATEINSTANCEEX*)
            GetProcAddress(gole32, "CoCreateInstanceEx");
    if (!gCoCreateInstanceEx)
        rb_raise(rb_eRuntimeError, "CoCreateInstanceEx is not supported in this environment");

    pbuf  = ole_vstr2wc(ole);
    hr = CLSIDFromProgID(pbuf, &clsid);
    if (FAILED(hr))
        hr = clsid_from_remote(host, ole, &clsid);
    if (FAILED(hr))
        hr = CLSIDFromString(pbuf, &clsid);
    SysFreeString(pbuf);
    if (FAILED(hr))
        ole_raise(hr, eWIN32OLERuntimeError,
                  "unknown OLE server: `%s'",
                  StringValuePtr(ole));
    memset(&serverinfo, 0, sizeof(COSERVERINFO));
    serverinfo.pwszName = ole_vstr2wc(host);
    memset(&multi_qi, 0, sizeof(MULTI_QI));
    multi_qi.pIID = &IID_IDispatch;
    hr = gCoCreateInstanceEx(&clsid, NULL, clsctx, &serverinfo, 1, &multi_qi);
    SysFreeString(serverinfo.pwszName);
    if (FAILED(hr))
        ole_raise(hr, eWIN32OLERuntimeError,
                  "failed to create DCOM server `%s' in `%s'",
                  StringValuePtr(ole),
                  StringValuePtr(host));

    ole_set_member(self, (IDispatch*)multi_qi.pItf);
    return self;
}

static VALUE
ole_bind_obj(VALUE moniker, int argc, VALUE *argv, VALUE self)
{
    IBindCtx *pBindCtx;
    IMoniker *pMoniker;
    IDispatch *pDispatch;
    void *p;
    HRESULT hr;
    OLECHAR *pbuf;
    ULONG eaten = 0;

    ole_initialize();

    hr = CreateBindCtx(0, &pBindCtx);
    if(FAILED(hr)) {
        ole_raise(hr, eWIN32OLERuntimeError,
                  "failed to create bind context");
    }

    pbuf  = ole_vstr2wc(moniker);
    hr = MkParseDisplayName(pBindCtx, pbuf, &eaten, &pMoniker);
    SysFreeString(pbuf);
    if(FAILED(hr)) {
        OLE_RELEASE(pBindCtx);
        ole_raise(hr, eWIN32OLERuntimeError,
                  "failed to parse display name of moniker `%s'",
                  StringValuePtr(moniker));
    }
    hr = pMoniker->lpVtbl->BindToObject(pMoniker, pBindCtx, NULL,
                                        &IID_IDispatch, &p);
    pDispatch = p;
    OLE_RELEASE(pMoniker);
    OLE_RELEASE(pBindCtx);

    if(FAILED(hr)) {
        ole_raise(hr, eWIN32OLERuntimeError,
                  "failed to bind moniker `%s'",
                  StringValuePtr(moniker));
    }
    return create_win32ole_object(self, pDispatch, argc, argv);
}

/*
 *  call-seq:
 *     WIN32OLE.connect( ole ) --> aWIN32OLE
 *
 *  Returns running OLE Automation object or WIN32OLE object from moniker.
 *  1st argument should be OLE program id or class id or moniker.
 *
 *     WIN32OLE.connect('Excel.Application') # => WIN32OLE object which represents running Excel.
 */
static VALUE
fole_s_connect(int argc, VALUE *argv, VALUE self)
{
    VALUE svr_name;
    VALUE others;
    HRESULT hr;
    CLSID   clsid;
    OLECHAR *pBuf;
    IDispatch *pDispatch;
    void *p;
    IUnknown *pUnknown;

    /* initialize to use OLE */
    ole_initialize();

    rb_scan_args(argc, argv, "1*", &svr_name, &others);
    SafeStringValue(svr_name);
    if (rb_safe_level() > 0 && OBJ_TAINTED(svr_name)) {
        rb_raise(rb_eSecurityError, "Insecure Object Connection - %s",
		 StringValuePtr(svr_name));
    }

    /* get CLSID from OLE server name */
    pBuf = ole_vstr2wc(svr_name);
    hr = CLSIDFromProgID(pBuf, &clsid);
    if(FAILED(hr)) {
        hr = CLSIDFromString(pBuf, &clsid);
    }
    SysFreeString(pBuf);
    if(FAILED(hr)) {
        return ole_bind_obj(svr_name, argc, argv, self);
    }

    hr = GetActiveObject(&clsid, 0, &pUnknown);
    if (FAILED(hr)) {
        ole_raise(hr, eWIN32OLERuntimeError,
                  "OLE server `%s' not running", StringValuePtr(svr_name));
    }
    hr = pUnknown->lpVtbl->QueryInterface(pUnknown, &IID_IDispatch, &p);
    pDispatch = p;
    if(FAILED(hr)) {
        OLE_RELEASE(pUnknown);
        ole_raise(hr, eWIN32OLERuntimeError,
                  "failed to create WIN32OLE server `%s'",
                  StringValuePtr(svr_name));
    }

    OLE_RELEASE(pUnknown);

    return create_win32ole_object(self, pDispatch, argc, argv);
}

/*
 *  call-seq:
 *     WIN32OLE.const_load( ole, mod = WIN32OLE)
 *
 *  Defines the constants of OLE Automation server as mod's constants.
 *  The first argument is WIN32OLE object or type library name.
 *  If 2nd argument is omitted, the default is WIN32OLE.
 *  The first letter of Ruby's constant variable name is upper case,
 *  so constant variable name of WIN32OLE object is capitalized.
 *  For example, the 'xlTop' constant of Excel is changed to 'XlTop'
 *  in WIN32OLE.
 *  If the first letter of constant variable is not [A-Z], then
 *  the constant is defined as CONSTANTS hash element.
 *
 *     module EXCEL_CONST
 *     end
 *     excel = WIN32OLE.new('Excel.Application')
 *     WIN32OLE.const_load(excel, EXCEL_CONST)
 *     puts EXCEL_CONST::XlTop # => -4160
 *     puts EXCEL_CONST::CONSTANTS['_xlDialogChartSourceData'] # => 541
 *
 *     WIN32OLE.const_load(excel)
 *     puts WIN32OLE::XlTop # => -4160
 *
 *     module MSO
 *     end
 *     WIN32OLE.const_load('Microsoft Office 9.0 Object Library', MSO)
 *     puts MSO::MsoLineSingle # => 1
 */
static VALUE
fole_s_const_load(int argc, VALUE *argv, VALUE self)
{
    VALUE ole;
    VALUE klass;
    struct oledata *pole;
    ITypeInfo *pTypeInfo;
    ITypeLib *pTypeLib;
    unsigned int index;
    HRESULT hr;
    OLECHAR *pBuf;
    VALUE file;
    LCID    lcid = cWIN32OLE_lcid;

    rb_scan_args(argc, argv, "11", &ole, &klass);
    if (!RB_TYPE_P(klass, T_CLASS) &&
        !RB_TYPE_P(klass, T_MODULE) &&
        !RB_TYPE_P(klass, T_NIL)) {
        rb_raise(rb_eTypeError, "2nd parameter must be Class or Module");
    }
    if (rb_obj_is_kind_of(ole, cWIN32OLE)) {
        OLEData_Get_Struct(ole, pole);
        hr = pole->pDispatch->lpVtbl->GetTypeInfo(pole->pDispatch,
                                                  0, lcid, &pTypeInfo);
        if(FAILED(hr)) {
            ole_raise(hr, rb_eRuntimeError, "failed to GetTypeInfo");
        }
        hr = pTypeInfo->lpVtbl->GetContainingTypeLib(pTypeInfo, &pTypeLib, &index);
        if(FAILED(hr)) {
            OLE_RELEASE(pTypeInfo);
            ole_raise(hr, rb_eRuntimeError, "failed to GetContainingTypeLib");
        }
        OLE_RELEASE(pTypeInfo);
        if(!RB_TYPE_P(klass, T_NIL)) {
            ole_const_load(pTypeLib, klass, self);
        }
        else {
            ole_const_load(pTypeLib, cWIN32OLE, self);
        }
        OLE_RELEASE(pTypeLib);
    }
    else if(RB_TYPE_P(ole, T_STRING)) {
        file = typelib_file(ole);
        if (file == Qnil) {
            file = ole;
        }
        pBuf = ole_vstr2wc(file);
        hr = LoadTypeLibEx(pBuf, REGKIND_NONE, &pTypeLib);
        SysFreeString(pBuf);
        if (FAILED(hr))
          ole_raise(hr, eWIN32OLERuntimeError, "failed to LoadTypeLibEx");
        if(!RB_TYPE_P(klass, T_NIL)) {
            ole_const_load(pTypeLib, klass, self);
        }
        else {
            ole_const_load(pTypeLib, cWIN32OLE, self);
        }
        OLE_RELEASE(pTypeLib);
    }
    else {
        rb_raise(rb_eTypeError, "1st parameter must be WIN32OLE instance");
    }
    return Qnil;
}

static ULONG
reference_count(struct oledata * pole)
{
    ULONG n = 0;
    if(pole->pDispatch) {
        OLE_ADDREF(pole->pDispatch);
        n = OLE_RELEASE(pole->pDispatch);
    }
    return n;
}

/*
 *  call-seq:
 *     WIN32OLE.ole_reference_count(aWIN32OLE) --> number
 *
 *  Returns reference counter of Dispatch interface of WIN32OLE object.
 *  You should not use this method because this method
 *  exists only for debugging WIN32OLE.
 */
static VALUE
fole_s_reference_count(VALUE self, VALUE obj)
{
    struct oledata * pole;
    OLEData_Get_Struct(obj, pole);
    return INT2NUM(reference_count(pole));
}

/*
 *  call-seq:
 *     WIN32OLE.ole_free(aWIN32OLE) --> number
 *
 *  Invokes Release method of Dispatch interface of WIN32OLE object.
 *  You should not use this method because this method
 *  exists only for debugging WIN32OLE.
 *  The return value is reference counter of OLE object.
 */
static VALUE
fole_s_free(VALUE self, VALUE obj)
{
    ULONG n = 0;
    struct oledata * pole;
    OLEData_Get_Struct(obj, pole);
    if(pole->pDispatch) {
        if (reference_count(pole) > 0) {
            n = OLE_RELEASE(pole->pDispatch);
        }
    }
    return INT2NUM(n);
}

static HWND
ole_show_help(VALUE helpfile, VALUE helpcontext)
{
    FNHTMLHELP *pfnHtmlHelp;
    HWND hwnd = 0;

    if(!ghhctrl)
        ghhctrl = LoadLibrary("HHCTRL.OCX");
    if (!ghhctrl)
        return hwnd;
    pfnHtmlHelp = (FNHTMLHELP*)GetProcAddress(ghhctrl, "HtmlHelpA");
    if (!pfnHtmlHelp)
        return hwnd;
    hwnd = pfnHtmlHelp(GetDesktopWindow(), StringValuePtr(helpfile),
                    0x0f, NUM2INT(helpcontext));
    if (hwnd == 0)
        hwnd = pfnHtmlHelp(GetDesktopWindow(), StringValuePtr(helpfile),
                 0,  NUM2INT(helpcontext));
    return hwnd;
}

/*
 *  call-seq:
 *     WIN32OLE.ole_show_help(obj [,helpcontext])
 *
 *  Displays helpfile. The 1st argument specifies WIN32OLE_TYPE
 *  object or WIN32OLE_METHOD object or helpfile.
 *
 *     excel = WIN32OLE.new('Excel.Application')
 *     typeobj = excel.ole_type
 *     WIN32OLE.ole_show_help(typeobj)
 */
static VALUE
fole_s_show_help(int argc, VALUE *argv, VALUE self)
{
    VALUE target;
    VALUE helpcontext;
    VALUE helpfile;
    VALUE name;
    HWND  hwnd;
    rb_scan_args(argc, argv, "11", &target, &helpcontext);
    if (rb_obj_is_kind_of(target, cWIN32OLE_TYPE) ||
        rb_obj_is_kind_of(target, cWIN32OLE_METHOD)) {
        helpfile = rb_funcall(target, rb_intern("helpfile"), 0);
        if(strlen(StringValuePtr(helpfile)) == 0) {
            name = rb_ivar_get(target, rb_intern("name"));
            rb_raise(rb_eRuntimeError, "no helpfile of `%s'",
                     StringValuePtr(name));
        }
        helpcontext = rb_funcall(target, rb_intern("helpcontext"), 0);
    } else {
        helpfile = target;
    }
    if (!RB_TYPE_P(helpfile, T_STRING)) {
        rb_raise(rb_eTypeError, "1st parameter must be (String|WIN32OLE_TYPE|WIN32OLE_METHOD)");
    }
    hwnd = ole_show_help(helpfile, helpcontext);
    if(hwnd == 0) {
        rb_raise(rb_eRuntimeError, "failed to open help file `%s'",
                 StringValuePtr(helpfile));
    }
    return Qnil;
}

/*
 *  call-seq:
 *     WIN32OLE.codepage
 *
 *  Returns current codepage.
 *     WIN32OLE.codepage # => WIN32OLE::CP_ACP
 */
static VALUE
fole_s_get_code_page(VALUE self)
{
    return INT2FIX(cWIN32OLE_cp);
}

static BOOL CALLBACK
installed_code_page_proc(LPTSTR str) {
    if (strtoul(str, NULL, 10) == g_cp_to_check) {
        g_cp_installed = TRUE;
        return FALSE;
    }
    return TRUE;
}

static BOOL
code_page_installed(UINT cp)
{
    g_cp_installed = FALSE;
    g_cp_to_check = cp;
    EnumSystemCodePages(installed_code_page_proc, CP_INSTALLED);
    return g_cp_installed;
}

/*
 *  call-seq:
 *     WIN32OLE.codepage = CP
 *
 *  Sets current codepage.
 *  The WIN32OLE.codepage is initialized according to
 *  Encoding.default_internal.
 *  If Encoding.default_internal is nil then WIN32OLE.codepage
 *  is initialized according to Encoding.default_external.
 *
 *     WIN32OLE.codepage = WIN32OLE::CP_UTF8
 *     WIN32OLE.codepage = 65001
 */
static VALUE
fole_s_set_code_page(VALUE self, VALUE vcp)
{
    UINT cp = FIX2INT(vcp);
    set_ole_codepage(cp);
    /*
     * Should this method return old codepage?
     */
    return Qnil;
}

/*
 *  call-seq:
 *     WIN32OLE.locale -> locale id.
 *
 *  Returns current locale id (lcid). The default locale is
 *  WIN32OLE::LOCALE_SYSTEM_DEFAULT.
 *
 *     lcid = WIN32OLE.locale
 */
static VALUE
fole_s_get_locale(VALUE self)
{
    return INT2FIX(cWIN32OLE_lcid);
}

static BOOL
CALLBACK installed_lcid_proc(LPTSTR str)
{
    if (strcmp(str, g_lcid_to_check) == 0) {
        g_lcid_installed = TRUE;
        return FALSE;
    }
    return TRUE;
}

static BOOL
lcid_installed(LCID lcid)
{
    g_lcid_installed = FALSE;
    snprintf(g_lcid_to_check, sizeof(g_lcid_to_check), "%08lx", (unsigned long)lcid);
    EnumSystemLocales(installed_lcid_proc, LCID_INSTALLED);
    return g_lcid_installed;
}

/*
 *  call-seq:
 *     WIN32OLE.locale = lcid
 *
 *  Sets current locale id (lcid).
 *
 *     WIN32OLE.locale = 1033 # set locale English(U.S)
 *     obj = WIN32OLE_VARIANT.new("$100,000", WIN32OLE::VARIANT::VT_CY)
 *
 */
static VALUE
fole_s_set_locale(VALUE self, VALUE vlcid)
{
    LCID lcid = FIX2INT(vlcid);
    if (lcid_installed(lcid)) {
        cWIN32OLE_lcid = lcid;
    } else {
        switch (lcid) {
        case LOCALE_SYSTEM_DEFAULT:
        case LOCALE_USER_DEFAULT:
            cWIN32OLE_lcid = lcid;
            break;
        default:
            rb_raise(eWIN32OLERuntimeError, "not installed locale: %u", (unsigned int)lcid);
        }
    }
    return Qnil;
}

/*
 *  call-seq:
 *     WIN32OLE.create_guid
 *
 *  Creates GUID.
 *     WIN32OLE.create_guid # => {1CB530F1-F6B1-404D-BCE6-1959BF91F4A8}
 */
static VALUE
fole_s_create_guid(VALUE self)
{
    GUID guid;
    HRESULT hr;
    OLECHAR bstr[80];
    int len = 0;
    hr = CoCreateGuid(&guid);
    if (FAILED(hr)) {
        ole_raise(hr, eWIN32OLERuntimeError, "failed to create GUID");
    }
    len = StringFromGUID2(&guid, bstr, sizeof(bstr)/sizeof(OLECHAR));
    if (len == 0) {
        rb_raise(rb_eRuntimeError, "failed to create GUID(buffer over)");
    }
    return ole_wc2vstr(bstr, FALSE);
}

/*
 * WIN32OLE.ole_initialize and WIN32OLE.ole_uninitialize
 * are used in win32ole.rb to fix the issue bug #2618 (ruby-core:27634).
 * You must not use these method.
 */

/* :nodoc */
static VALUE
fole_s_ole_initialize(VALUE self)
{
    ole_initialize();
    return Qnil;
}

/* :nodoc */
static VALUE
fole_s_ole_uninitialize(VALUE self)
{
    ole_uninitialize();
    return Qnil;
}

/*
 * Document-class: WIN32OLE
 *
 *   <code>WIN32OLE</code> objects represent OLE Automation object in Ruby.
 *
 *   By using WIN32OLE, you can access OLE server like VBScript.
 *
 *   Here is sample script.
 *
 *     require 'win32ole'
 *
 *     excel = WIN32OLE.new('Excel.Application')
 *     excel.visible = true
 *     workbook = excel.Workbooks.Add();
 *     worksheet = workbook.Worksheets(1);
 *     worksheet.Range("A1:D1").value = ["North","South","East","West"];
 *     worksheet.Range("A2:B2").value = [5.2, 10];
 *     worksheet.Range("C2").value = 8;
 *     worksheet.Range("D2").value = 20;
 *
 *     range = worksheet.Range("A1:D2");
 *     range.select
 *     chart = workbook.Charts.Add;
 *
 *     workbook.saved = true;
 *
 *     excel.ActiveWorkbook.Close(0);
 *     excel.Quit();
 *
 *   Unfortunately, Win32OLE doesn't support the argument passed by
 *   reference directly.
 *   Instead, Win32OLE provides WIN32OLE::ARGV or WIN32OLE_VARIANT object.
 *   If you want to get the result value of argument passed by reference,
 *   you can use WIN32OLE::ARGV or WIN32OLE_VARIANT.
 *
 *     oleobj.method(arg1, arg2, refargv3)
 *     puts WIN32OLE::ARGV[2]   # the value of refargv3 after called oleobj.method
 *
 *   or
 *
 *     refargv3 = WIN32OLE_VARIANT.new(XXX,
 *                 WIN32OLE::VARIANT::VT_BYREF|WIN32OLE::VARIANT::VT_XXX)
 *     oleobj.method(arg1, arg2, refargv3)
 *     p refargv3.value # the value of refargv3 after called oleobj.method.
 *
 */

/*
 *  call-seq:
 *     WIN32OLE.new(server, [host]) -> WIN32OLE object
 *
 *  Returns a new WIN32OLE object(OLE Automation object).
 *  The first argument server specifies OLE Automation server.
 *  The first argument should be CLSID or PROGID.
 *  If second argument host specified, then returns OLE Automation
 *  object on host.
 *
 *      WIN32OLE.new('Excel.Application') # => Excel OLE Automation WIN32OLE object.
 *      WIN32OLE.new('{00024500-0000-0000-C000-000000000046}') # => Excel OLE Automation WIN32OLE object.
 */
static VALUE
fole_initialize(int argc, VALUE *argv, VALUE self)
{
    VALUE svr_name;
    VALUE host;
    VALUE others;
    HRESULT hr;
    CLSID   clsid;
    OLECHAR *pBuf;
    IDispatch *pDispatch;
    void *p;
    rb_call_super(0, 0);
    rb_scan_args(argc, argv, "11*", &svr_name, &host, &others);

    SafeStringValue(svr_name);
    if (rb_safe_level() > 0 && OBJ_TAINTED(svr_name)) {
        rb_raise(rb_eSecurityError, "Insecure Object Creation - %s",
                 StringValuePtr(svr_name));
    }
    if (!NIL_P(host)) {
	SafeStringValue(host);
        if (rb_safe_level() > 0 && OBJ_TAINTED(host)) {
            rb_raise(rb_eSecurityError, "Insecure Object Creation - %s",
                     StringValuePtr(svr_name));
        }
        return ole_create_dcom(self, svr_name, host, others);
    }

    /* get CLSID from OLE server name */
    pBuf  = ole_vstr2wc(svr_name);
    hr = CLSIDFromProgID(pBuf, &clsid);
    if(FAILED(hr)) {
        hr = CLSIDFromString(pBuf, &clsid);
    }
    SysFreeString(pBuf);
    if(FAILED(hr)) {
        ole_raise(hr, eWIN32OLERuntimeError,
                  "unknown OLE server: `%s'",
                  StringValuePtr(svr_name));
    }

    /* get IDispatch interface */
    hr = CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER,
                          &IID_IDispatch, &p);
    pDispatch = p;
    if(FAILED(hr)) {
        ole_raise(hr, eWIN32OLERuntimeError,
                  "failed to create WIN32OLE object from `%s'",
                  StringValuePtr(svr_name));
    }

    ole_set_member(self, pDispatch);
    return self;
}

static int
hash2named_arg(VALUE key, VALUE val, VALUE pop)
{
    struct oleparam* pOp = (struct oleparam *)pop;
    unsigned int index, i;
    index = pOp->dp.cNamedArgs;
    /*---------------------------------------------
      the data-type of key must be String or Symbol
    -----------------------------------------------*/
    if(!RB_TYPE_P(key, T_STRING) && !RB_TYPE_P(key, T_SYMBOL)) {
        /* clear name of dispatch parameters */
        for(i = 1; i < index + 1; i++) {
            SysFreeString(pOp->pNamedArgs[i]);
        }
        /* clear dispatch parameters */
        for(i = 0; i < index; i++ ) {
            VariantClear(&(pOp->dp.rgvarg[i]));
        }
        /* raise an exception */
        rb_raise(rb_eTypeError, "wrong argument type (expected String or Symbol)");
    }
    if (RB_TYPE_P(key, T_SYMBOL)) {
	key = rb_sym_to_s(key);
    }

    /* pNamedArgs[0] is <method name>, so "index + 1" */
    pOp->pNamedArgs[index + 1] = ole_vstr2wc(key);

    VariantInit(&(pOp->dp.rgvarg[index]));
    ole_val2variant(val, &(pOp->dp.rgvarg[index]));

    pOp->dp.cNamedArgs += 1;
    return ST_CONTINUE;
}

static VALUE
set_argv(VARIANTARG* realargs, unsigned int beg, unsigned int end)
{
    VALUE argv = rb_const_get(cWIN32OLE, rb_intern("ARGV"));

    Check_Type(argv, T_ARRAY);
    rb_ary_clear(argv);
    while (end-- > beg) {
        rb_ary_push(argv, ole_variant2val(&realargs[end]));
        if (V_VT(&realargs[end]) != VT_RECORD) {
            VariantClear(&realargs[end]);
        }
    }
    return argv;
}

static VALUE
ole_invoke(int argc, VALUE *argv, VALUE self, USHORT wFlags, BOOL is_bracket)
{
    LCID    lcid = cWIN32OLE_lcid;
    struct oledata *pole;
    HRESULT hr;
    VALUE cmd;
    VALUE paramS;
    VALUE param;
    VALUE obj;
    VALUE v;

    BSTR wcmdname;

    DISPID DispID;
    DISPID* pDispID;
    EXCEPINFO excepinfo;
    VARIANT result;
    VARIANTARG* realargs = NULL;
    unsigned int argErr = 0;
    unsigned int i;
    unsigned int cNamedArgs;
    int n;
    struct oleparam op;
    struct olevariantdata *pvar;
    memset(&excepinfo, 0, sizeof(EXCEPINFO));

    VariantInit(&result);

    op.dp.rgvarg = NULL;
    op.dp.rgdispidNamedArgs = NULL;
    op.dp.cNamedArgs = 0;
    op.dp.cArgs = 0;

    rb_scan_args(argc, argv, "1*", &cmd, &paramS);
    if(!RB_TYPE_P(cmd, T_STRING) && !RB_TYPE_P(cmd, T_SYMBOL) && !is_bracket) {
	rb_raise(rb_eTypeError, "method is wrong type (expected String or Symbol)");
    }
    if (RB_TYPE_P(cmd, T_SYMBOL)) {
	cmd = rb_sym_to_s(cmd);
    }
    OLEData_Get_Struct(self, pole);
    if(!pole->pDispatch) {
        rb_raise(rb_eRuntimeError, "failed to get dispatch interface");
    }
    if (is_bracket) {
        DispID = DISPID_VALUE;
        argc += 1;
	rb_ary_unshift(paramS, cmd);
    } else {
        wcmdname = ole_vstr2wc(cmd);
        hr = pole->pDispatch->lpVtbl->GetIDsOfNames( pole->pDispatch, &IID_NULL,
                &wcmdname, 1, lcid, &DispID);
        SysFreeString(wcmdname);
        if(FAILED(hr)) {
            ole_raise(hr, rb_eNoMethodError,
                    "unknown property or method: `%s'",
                    StringValuePtr(cmd));
        }
    }

    /* pick up last argument of method */
    param = rb_ary_entry(paramS, argc-2);

    op.dp.cNamedArgs = 0;

    /* if last arg is hash object */
    if(RB_TYPE_P(param, T_HASH)) {
        /*------------------------------------------
          hash object ==> named dispatch parameters
        --------------------------------------------*/
        cNamedArgs = NUM2INT(rb_funcall(param, rb_intern("length"), 0));
        op.dp.cArgs = cNamedArgs + argc - 2;
        op.pNamedArgs = ALLOCA_N(OLECHAR*, cNamedArgs + 1);
        op.dp.rgvarg = ALLOCA_N(VARIANTARG, op.dp.cArgs);

        rb_hash_foreach(param, hash2named_arg, (VALUE)&op);

        pDispID = ALLOCA_N(DISPID, cNamedArgs + 1);
        op.pNamedArgs[0] = ole_vstr2wc(cmd);
        hr = pole->pDispatch->lpVtbl->GetIDsOfNames(pole->pDispatch,
                                                    &IID_NULL,
                                                    op.pNamedArgs,
                                                    op.dp.cNamedArgs + 1,
                                                    lcid, pDispID);
        for(i = 0; i < op.dp.cNamedArgs + 1; i++) {
            SysFreeString(op.pNamedArgs[i]);
            op.pNamedArgs[i] = NULL;
        }
        if(FAILED(hr)) {
            /* clear dispatch parameters */
            for(i = 0; i < op.dp.cArgs; i++ ) {
                VariantClear(&op.dp.rgvarg[i]);
            }
            ole_raise(hr, eWIN32OLERuntimeError,
                      "failed to get named argument info: `%s'",
                      StringValuePtr(cmd));
        }
        op.dp.rgdispidNamedArgs = &(pDispID[1]);
    }
    else {
        cNamedArgs = 0;
        op.dp.cArgs = argc - 1;
        op.pNamedArgs = ALLOCA_N(OLECHAR*, cNamedArgs + 1);
        if (op.dp.cArgs > 0) {
            op.dp.rgvarg  = ALLOCA_N(VARIANTARG, op.dp.cArgs);
        }
    }
    /*--------------------------------------
      non hash args ==> dispatch parameters
     ----------------------------------------*/
    if(op.dp.cArgs > cNamedArgs) {
        realargs = ALLOCA_N(VARIANTARG, op.dp.cArgs-cNamedArgs+1);
        for(i = cNamedArgs; i < op.dp.cArgs; i++) {
            n = op.dp.cArgs - i + cNamedArgs - 1;
            VariantInit(&realargs[n]);
            VariantInit(&op.dp.rgvarg[n]);
            param = rb_ary_entry(paramS, i-cNamedArgs);
            if (rb_obj_is_kind_of(param, cWIN32OLE_VARIANT)) {
                Data_Get_Struct(param, struct olevariantdata, pvar);
                VariantCopy(&op.dp.rgvarg[n], &(pvar->var));
            } else {
                ole_val2variant(param, &realargs[n]);
                V_VT(&op.dp.rgvarg[n]) = VT_VARIANT | VT_BYREF;
                V_VARIANTREF(&op.dp.rgvarg[n]) = &realargs[n];
            }
        }
    }
    /* apparent you need to call propput, you need this */
    if (wFlags & DISPATCH_PROPERTYPUT) {
        if (op.dp.cArgs == 0)
            ole_raise(ResultFromScode(E_INVALIDARG), eWIN32OLERuntimeError, "argument error");

        op.dp.cNamedArgs = 1;
        op.dp.rgdispidNamedArgs = ALLOCA_N( DISPID, 1 );
        op.dp.rgdispidNamedArgs[0] = DISPID_PROPERTYPUT;
    }
    hr = pole->pDispatch->lpVtbl->Invoke(pole->pDispatch, DispID,
                                         &IID_NULL, lcid, wFlags, &op.dp,
                                         &result, &excepinfo, &argErr);

    if (FAILED(hr)) {
        /* retry to call args by value */
        if(op.dp.cArgs >= cNamedArgs) {
            for(i = cNamedArgs; i < op.dp.cArgs; i++) {
                n = op.dp.cArgs - i + cNamedArgs - 1;
                param = rb_ary_entry(paramS, i-cNamedArgs);
                ole_val2variant(param, &op.dp.rgvarg[n]);
            }
            if (hr == DISP_E_EXCEPTION) {
                ole_freeexceptinfo(&excepinfo);
            }
            memset(&excepinfo, 0, sizeof(EXCEPINFO));
            VariantInit(&result);
            hr = pole->pDispatch->lpVtbl->Invoke(pole->pDispatch, DispID,
                                                 &IID_NULL, lcid, wFlags,
                                                 &op.dp, &result,
                                                 &excepinfo, &argErr);

            /* mega kludge. if a method in WORD is called and we ask
             * for a result when one is not returned then
             * hResult == DISP_E_EXCEPTION. this only happens on
             * functions whose DISPID > 0x8000 */
            if ((hr == DISP_E_EXCEPTION || hr == DISP_E_MEMBERNOTFOUND) && DispID > 0x8000) {
                if (hr == DISP_E_EXCEPTION) {
                    ole_freeexceptinfo(&excepinfo);
                }
                memset(&excepinfo, 0, sizeof(EXCEPINFO));
                hr = pole->pDispatch->lpVtbl->Invoke(pole->pDispatch, DispID,
                        &IID_NULL, lcid, wFlags,
                        &op.dp, NULL,
                        &excepinfo, &argErr);

            }
            for(i = cNamedArgs; i < op.dp.cArgs; i++) {
                n = op.dp.cArgs - i + cNamedArgs - 1;
                if (V_VT(&op.dp.rgvarg[n]) != VT_RECORD) {
                    VariantClear(&op.dp.rgvarg[n]);
                }
            }
        }

        if (FAILED(hr)) {
            /* retry after converting nil to VT_EMPTY */
            if (op.dp.cArgs > cNamedArgs) {
                for(i = cNamedArgs; i < op.dp.cArgs; i++) {
                    n = op.dp.cArgs - i + cNamedArgs - 1;
                    param = rb_ary_entry(paramS, i-cNamedArgs);
                    ole_val2variant2(param, &op.dp.rgvarg[n]);
                }
                if (hr == DISP_E_EXCEPTION) {
                    ole_freeexceptinfo(&excepinfo);
                }
                memset(&excepinfo, 0, sizeof(EXCEPINFO));
                VariantInit(&result);
                hr = pole->pDispatch->lpVtbl->Invoke(pole->pDispatch, DispID,
                        &IID_NULL, lcid, wFlags,
                        &op.dp, &result,
                        &excepinfo, &argErr);
                for(i = cNamedArgs; i < op.dp.cArgs; i++) {
                    n = op.dp.cArgs - i + cNamedArgs - 1;
                    if (V_VT(&op.dp.rgvarg[n]) != VT_RECORD) {
                        VariantClear(&op.dp.rgvarg[n]);
                    }
                }
            }
        }

    }
    /* clear dispatch parameter */
    if(op.dp.cArgs > cNamedArgs) {
        for(i = cNamedArgs; i < op.dp.cArgs; i++) {
            n = op.dp.cArgs - i + cNamedArgs - 1;
            param = rb_ary_entry(paramS, i-cNamedArgs);
            if (rb_obj_is_kind_of(param, cWIN32OLE_VARIANT)) {
                ole_val2variant(param, &realargs[n]);
            } else if ( rb_obj_is_kind_of(param, cWIN32OLE_RECORD) &&
                        V_VT(&realargs[n]) == VT_RECORD ) {
                olerecord_set_ivar(param, V_RECORDINFO(&realargs[n]), V_RECORD(&realargs[n]));
            }
        }
        set_argv(realargs, cNamedArgs, op.dp.cArgs);
    }
    else {
        for(i = 0; i < op.dp.cArgs; i++) {
            VariantClear(&op.dp.rgvarg[i]);
        }
    }

    if (FAILED(hr)) {
        v = ole_excepinfo2msg(&excepinfo);
        ole_raise(hr, eWIN32OLERuntimeError, "(in OLE method `%s': )%s",
                  StringValuePtr(cmd),
                  StringValuePtr(v));
    }
    obj = ole_variant2val(&result);
    VariantClear(&result);
    return obj;
}

/*
 *  call-seq:
 *     WIN32OLE#invoke(method, [arg1,...])  => return value of method.
 *
 *  Runs OLE method.
 *  The first argument specifies the method name of OLE Automation object.
 *  The others specify argument of the <i>method</i>.
 *  If you can not execute <i>method</i> directly, then use this method instead.
 *
 *    excel = WIN32OLE.new('Excel.Application')
 *    excel.invoke('Quit')  # => same as excel.Quit
 *
 */
static VALUE
fole_invoke(int argc, VALUE *argv, VALUE self)
{
    return ole_invoke(argc, argv, self, DISPATCH_METHOD|DISPATCH_PROPERTYGET, FALSE);
}

static VALUE
ole_invoke2(VALUE self, VALUE dispid, VALUE args, VALUE types, USHORT dispkind)
{
    HRESULT hr;
    struct oledata *pole;
    unsigned int argErr = 0;
    EXCEPINFO excepinfo;
    VARIANT result;
    DISPPARAMS dispParams;
    VARIANTARG* realargs = NULL;
    int i, j;
    VALUE obj = Qnil;
    VALUE tp, param;
    VALUE v;
    VARTYPE vt;

    Check_Type(args, T_ARRAY);
    Check_Type(types, T_ARRAY);

    memset(&excepinfo, 0, sizeof(EXCEPINFO));
    memset(&dispParams, 0, sizeof(DISPPARAMS));
    VariantInit(&result);
    OLEData_Get_Struct(self, pole);

    dispParams.cArgs = RARRAY_LEN(args);
    dispParams.rgvarg = ALLOCA_N(VARIANTARG, dispParams.cArgs);
    realargs = ALLOCA_N(VARIANTARG, dispParams.cArgs);
    for (i = 0, j = dispParams.cArgs - 1; i < (int)dispParams.cArgs; i++, j--)
    {
        VariantInit(&realargs[i]);
        VariantInit(&dispParams.rgvarg[i]);
        tp = rb_ary_entry(types, j);
        vt = (VARTYPE)FIX2INT(tp);
        V_VT(&dispParams.rgvarg[i]) = vt;
        param = rb_ary_entry(args, j);
        if (param == Qnil)
        {

            V_VT(&dispParams.rgvarg[i]) = V_VT(&realargs[i]) = VT_ERROR;
            V_ERROR(&dispParams.rgvarg[i]) = V_ERROR(&realargs[i]) = DISP_E_PARAMNOTFOUND;
        }
        else
        {
            if (vt & VT_ARRAY)
            {
                int ent;
                LPBYTE pb;
                short* ps;
                LPLONG pl;
                VARIANT* pv;
                CY *py;
                VARTYPE v;
                SAFEARRAYBOUND rgsabound[1];
                Check_Type(param, T_ARRAY);
                rgsabound[0].lLbound = 0;
                rgsabound[0].cElements = RARRAY_LEN(param);
                v = vt & ~(VT_ARRAY | VT_BYREF);
                V_ARRAY(&realargs[i]) = SafeArrayCreate(v, 1, rgsabound);
                V_VT(&realargs[i]) = VT_ARRAY | v;
                SafeArrayLock(V_ARRAY(&realargs[i]));
                pb = V_ARRAY(&realargs[i])->pvData;
                ps = V_ARRAY(&realargs[i])->pvData;
                pl = V_ARRAY(&realargs[i])->pvData;
                py = V_ARRAY(&realargs[i])->pvData;
                pv = V_ARRAY(&realargs[i])->pvData;
                for (ent = 0; ent < (int)rgsabound[0].cElements; ent++)
                {
                    VARIANT velem;
                    VALUE elem = rb_ary_entry(param, ent);
                    ole_val2variant(elem, &velem);
                    if (v != VT_VARIANT)
                    {
                        VariantChangeTypeEx(&velem, &velem,
                            cWIN32OLE_lcid, 0, v);
                    }
                    switch (v)
                    {
                    /* 128 bits */
                    case VT_VARIANT:
                        *pv++ = velem;
                        break;
                    /* 64 bits */
                    case VT_R8:
                    case VT_CY:
                    case VT_DATE:
                        *py++ = V_CY(&velem);
                        break;
                    /* 16 bits */
                    case VT_BOOL:
                    case VT_I2:
                    case VT_UI2:
                        *ps++ = V_I2(&velem);
                        break;
                    /* 8 bites */
                    case VT_UI1:
                    case VT_I1:
                        *pb++ = V_UI1(&velem);
                        break;
                    /* 32 bits */
                    default:
                        *pl++ = V_I4(&velem);
                        break;
                    }
                }
                SafeArrayUnlock(V_ARRAY(&realargs[i]));
            }
            else
            {
                ole_val2variant(param, &realargs[i]);
                if ((vt & (~VT_BYREF)) != VT_VARIANT)
                {
                    hr = VariantChangeTypeEx(&realargs[i], &realargs[i],
                                             cWIN32OLE_lcid, 0,
                                             (VARTYPE)(vt & (~VT_BYREF)));
                    if (hr != S_OK)
                    {
                        rb_raise(rb_eTypeError, "not valid value");
                    }
                }
            }
            if ((vt & VT_BYREF) || vt == VT_VARIANT)
            {
                if (vt == VT_VARIANT)
                    V_VT(&dispParams.rgvarg[i]) = VT_VARIANT | VT_BYREF;
                switch (vt & (~VT_BYREF))
                {
                /* 128 bits */
                case VT_VARIANT:
                    V_VARIANTREF(&dispParams.rgvarg[i]) = &realargs[i];
                    break;
                /* 64 bits */
                case VT_R8:
                case VT_CY:
                case VT_DATE:
                    V_CYREF(&dispParams.rgvarg[i]) = &V_CY(&realargs[i]);
                    break;
                /* 16 bits */
                case VT_BOOL:
                case VT_I2:
                case VT_UI2:
                    V_I2REF(&dispParams.rgvarg[i]) = &V_I2(&realargs[i]);
                    break;
                /* 8 bites */
                case VT_UI1:
                case VT_I1:
                    V_UI1REF(&dispParams.rgvarg[i]) = &V_UI1(&realargs[i]);
                    break;
                /* 32 bits */
                default:
                    V_I4REF(&dispParams.rgvarg[i]) = &V_I4(&realargs[i]);
                    break;
                }
            }
            else
            {
                /* copy 64 bits of data */
                V_CY(&dispParams.rgvarg[i]) = V_CY(&realargs[i]);
            }
        }
    }

    if (dispkind & DISPATCH_PROPERTYPUT) {
        dispParams.cNamedArgs = 1;
        dispParams.rgdispidNamedArgs = ALLOCA_N( DISPID, 1 );
        dispParams.rgdispidNamedArgs[0] = DISPID_PROPERTYPUT;
    }

    hr = pole->pDispatch->lpVtbl->Invoke(pole->pDispatch, NUM2INT(dispid),
                                         &IID_NULL, cWIN32OLE_lcid,
                                         dispkind,
                                         &dispParams, &result,
                                         &excepinfo, &argErr);

    if (FAILED(hr)) {
        v = ole_excepinfo2msg(&excepinfo);
        ole_raise(hr, eWIN32OLERuntimeError, "(in OLE method `<dispatch id:%d>': )%s",
                  NUM2INT(dispid),
                  StringValuePtr(v));
    }

    /* clear dispatch parameter */
    if(dispParams.cArgs > 0) {
        set_argv(realargs, 0, dispParams.cArgs);
    }

    obj = ole_variant2val(&result);
    VariantClear(&result);
    return obj;
}

/*
 *   call-seq:
 *      WIN32OLE#_invoke(dispid, args, types)
 *
 *   Runs the early binding method.
 *   The 1st argument specifies dispatch ID,
 *   the 2nd argument specifies the array of arguments,
 *   the 3rd argument specifies the array of the type of arguments.
 *
 *      excel = WIN32OLE.new('Excel.Application')
 *      excel._invoke(302, [], []) #  same effect as excel.Quit
 */
static VALUE
fole_invoke2(VALUE self, VALUE dispid, VALUE args, VALUE types)
{
    return ole_invoke2(self, dispid, args, types, DISPATCH_METHOD);
}

/*
 *  call-seq:
 *     WIN32OLE#_getproperty(dispid, args, types)
 *
 *  Runs the early binding method to get property.
 *  The 1st argument specifies dispatch ID,
 *  the 2nd argument specifies the array of arguments,
 *  the 3rd argument specifies the array of the type of arguments.
 *
 *     excel = WIN32OLE.new('Excel.Application')
 *     puts excel._getproperty(558, [], []) # same effect as puts excel.visible
 */
static VALUE
fole_getproperty2(VALUE self, VALUE dispid, VALUE args, VALUE types)
{
    return ole_invoke2(self, dispid, args, types, DISPATCH_PROPERTYGET);
}

/*
 *   call-seq:
 *      WIN32OLE#_setproperty(dispid, args, types)
 *
 *   Runs the early binding method to set property.
 *   The 1st argument specifies dispatch ID,
 *   the 2nd argument specifies the array of arguments,
 *   the 3rd argument specifies the array of the type of arguments.
 *
 *      excel = WIN32OLE.new('Excel.Application')
 *      excel._setproperty(558, [true], [WIN32OLE::VARIANT::VT_BOOL]) # same effect as excel.visible = true
 */
static VALUE
fole_setproperty2(VALUE self, VALUE dispid, VALUE args, VALUE types)
{
    return ole_invoke2(self, dispid, args, types, DISPATCH_PROPERTYPUT);
}

/*
 *  call-seq:
 *     WIN32OLE[a1, a2, ...]=val
 *
 *  Sets the value to WIN32OLE object specified by a1, a2, ...
 *
 *     dict = WIN32OLE.new('Scripting.Dictionary')
 *     dict.add('ruby', 'RUBY')
 *     dict['ruby'] = 'Ruby'
 *     puts dict['ruby'] # => 'Ruby'
 *
 *  Remark: You can not use this method to set the property value.
 *
 *     excel = WIN32OLE.new('Excel.Application')
 *     # excel['Visible'] = true # This is error !!!
 *     excel.Visible = true # You should to use this style to set the property.
 *
 */
static VALUE
fole_setproperty_with_bracket(int argc, VALUE *argv, VALUE self)
{
    return ole_invoke(argc, argv, self, DISPATCH_PROPERTYPUT, TRUE);
}

/*
 *  call-seq:
 *     WIN32OLE.setproperty('property', [arg1, arg2,...] val)
 *
 *  Sets property of OLE object.
 *  When you want to set property with argument, you can use this method.
 *
 *     excel = WIN32OLE.new('Excel.Application')
 *     excel.Visible = true
 *     book = excel.workbooks.add
 *     sheet = book.worksheets(1)
 *     sheet.setproperty('Cells', 1, 2, 10) # => The B1 cell value is 10.
 */
static VALUE
fole_setproperty(int argc, VALUE *argv, VALUE self)
{
    return ole_invoke(argc, argv, self, DISPATCH_PROPERTYPUT, FALSE);
}

/*
 *  call-seq:
 *     WIN32OLE[a1,a2,...]
 *
 *  Returns the value of Collection specified by a1, a2,....
 *
 *     dict = WIN32OLE.new('Scripting.Dictionary')
 *     dict.add('ruby', 'Ruby')
 *     puts dict['ruby'] # => 'Ruby' (same as `puts dict.item('ruby')')
 *
 *  Remark: You can not use this method to get the property.
 *     excel = WIN32OLE.new('Excel.Application')
 *     # puts excel['Visible']  This is error !!!
 *     puts excel.Visible # You should to use this style to get the property.
 *
 */
static VALUE
fole_getproperty_with_bracket(int argc, VALUE *argv, VALUE self)
{
    return ole_invoke(argc, argv, self, DISPATCH_PROPERTYGET, TRUE);
}

static VALUE
ole_propertyput(VALUE self, VALUE property, VALUE value)
{
    struct oledata *pole;
    unsigned argErr;
    unsigned int index;
    HRESULT hr;
    EXCEPINFO excepinfo;
    DISPID dispID = DISPID_VALUE;
    DISPID dispIDParam = DISPID_PROPERTYPUT;
    USHORT wFlags = DISPATCH_PROPERTYPUT|DISPATCH_PROPERTYPUTREF;
    DISPPARAMS dispParams;
    VARIANTARG propertyValue[2];
    OLECHAR* pBuf[1];
    VALUE v;
    LCID    lcid = cWIN32OLE_lcid;
    dispParams.rgdispidNamedArgs = &dispIDParam;
    dispParams.rgvarg = propertyValue;
    dispParams.cNamedArgs = 1;
    dispParams.cArgs = 1;

    VariantInit(&propertyValue[0]);
    VariantInit(&propertyValue[1]);
    memset(&excepinfo, 0, sizeof(excepinfo));

    OLEData_Get_Struct(self, pole);

    /* get ID from property name */
    pBuf[0]  = ole_vstr2wc(property);
    hr = pole->pDispatch->lpVtbl->GetIDsOfNames(pole->pDispatch, &IID_NULL,
                                                pBuf, 1, lcid, &dispID);
    SysFreeString(pBuf[0]);
    pBuf[0] = NULL;

    if(FAILED(hr)) {
        ole_raise(hr, eWIN32OLERuntimeError,
                  "unknown property or method: `%s'",
                  StringValuePtr(property));
    }
    /* set property value */
    ole_val2variant(value, &propertyValue[0]);
    hr = pole->pDispatch->lpVtbl->Invoke(pole->pDispatch, dispID, &IID_NULL,
                                         lcid, wFlags, &dispParams,
                                         NULL, &excepinfo, &argErr);

    for(index = 0; index < dispParams.cArgs; ++index) {
        VariantClear(&propertyValue[index]);
    }
    if (FAILED(hr)) {
        v = ole_excepinfo2msg(&excepinfo);
        ole_raise(hr, eWIN32OLERuntimeError, "(in setting property `%s': )%s",
                  StringValuePtr(property),
                  StringValuePtr(v));
    }
    return Qnil;
}

/*
 *  call-seq:
 *     WIN32OLE#ole_free
 *
 *  invokes Release method of Dispatch interface of WIN32OLE object.
 *  Usually, you do not need to call this method because Release method
 *  called automatically when WIN32OLE object garbaged.
 *
 */
static VALUE
fole_free(VALUE self)
{
    struct oledata *pole;
    OLEData_Get_Struct(self, pole);
    OLE_FREE(pole->pDispatch);
    pole->pDispatch = NULL;
    return Qnil;
}

static VALUE
ole_each_sub(VALUE pEnumV)
{
    VARIANT variant;
    VALUE obj = Qnil;
    IEnumVARIANT *pEnum = (IEnumVARIANT *)pEnumV;
    VariantInit(&variant);
    while(pEnum->lpVtbl->Next(pEnum, 1, &variant, NULL) == S_OK) {
        obj = ole_variant2val(&variant);
        VariantClear(&variant);
        VariantInit(&variant);
        rb_yield(obj);
    }
    return Qnil;
}

static VALUE
ole_ienum_free(VALUE pEnumV)
{
    IEnumVARIANT *pEnum = (IEnumVARIANT *)pEnumV;
    OLE_RELEASE(pEnum);
    return Qnil;
}

/*
 *  call-seq:
 *     WIN32OLE#each {|i|...}
 *
 *  Iterates over each item of OLE collection which has IEnumVARIANT interface.
 *
 *     excel = WIN32OLE.new('Excel.Application')
 *     book = excel.workbooks.add
 *     sheets = book.worksheets(1)
 *     cells = sheets.cells("A1:A5")
 *     cells.each do |cell|
 *       cell.value = 10
 *     end
 */
static VALUE
fole_each(VALUE self)
{
    LCID    lcid = cWIN32OLE_lcid;

    struct oledata *pole;

    unsigned int argErr;
    EXCEPINFO excepinfo;
    DISPPARAMS dispParams;
    VARIANT result;
    HRESULT hr;
    IEnumVARIANT *pEnum = NULL;
    void *p;

    RETURN_ENUMERATOR(self, 0, 0);

    VariantInit(&result);
    dispParams.rgvarg = NULL;
    dispParams.rgdispidNamedArgs = NULL;
    dispParams.cNamedArgs = 0;
    dispParams.cArgs = 0;
    memset(&excepinfo, 0, sizeof(excepinfo));

    OLEData_Get_Struct(self, pole);
    hr = pole->pDispatch->lpVtbl->Invoke(pole->pDispatch, DISPID_NEWENUM,
                                         &IID_NULL, lcid,
                                         DISPATCH_METHOD | DISPATCH_PROPERTYGET,
                                         &dispParams, &result,
                                         &excepinfo, &argErr);

    if (FAILED(hr)) {
        VariantClear(&result);
        ole_raise(hr, eWIN32OLERuntimeError, "failed to get IEnum Interface");
    }

    if (V_VT(&result) == VT_UNKNOWN) {
        hr = V_UNKNOWN(&result)->lpVtbl->QueryInterface(V_UNKNOWN(&result),
                                                        &IID_IEnumVARIANT,
                                                        &p);
        pEnum = p;
    } else if (V_VT(&result) == VT_DISPATCH) {
        hr = V_DISPATCH(&result)->lpVtbl->QueryInterface(V_DISPATCH(&result),
                                                         &IID_IEnumVARIANT,
                                                         &p);
        pEnum = p;
    }
    if (FAILED(hr) || !pEnum) {
        VariantClear(&result);
        ole_raise(hr, rb_eRuntimeError, "failed to get IEnum Interface");
    }

    VariantClear(&result);
    rb_ensure(ole_each_sub, (VALUE)pEnum, ole_ienum_free, (VALUE)pEnum);
    return Qnil;
}

/*
 *  call-seq:
 *     WIN32OLE#method_missing(id [,arg1, arg2, ...])
 *
 *  Calls WIN32OLE#invoke method.
 */
static VALUE
fole_missing(int argc, VALUE *argv, VALUE self)
{
    ID id;
    const char* mname;
    size_t n;
    rb_check_arity(argc, 1, UNLIMITED_ARGUMENTS);
    id = rb_to_id(argv[0]);
    mname = rb_id2name(id);
    if(!mname) {
        rb_raise(rb_eRuntimeError, "fail: unknown method or property");
    }
    n = strlen(mname);
#if SIZEOF_SIZE_T > SIZEOF_LONG
    if (n >= LONG_MAX) {
	rb_raise(rb_eRuntimeError, "too long method or property name");
    }
#endif
    if(mname[n-1] == '=') {
        rb_check_arity(argc, 2, 2);
        argv[0] = rb_enc_str_new(mname, (long)(n-1), cWIN32OLE_enc);

        return ole_propertyput(self, argv[0], argv[1]);
    }
    else {
        argv[0] = rb_enc_str_new(mname, (long)n, cWIN32OLE_enc);
        return ole_invoke(argc, argv, self, DISPATCH_METHOD|DISPATCH_PROPERTYGET, FALSE);
    }
}

static HRESULT
typeinfo_from_ole(struct oledata *pole, ITypeInfo **ppti)
{
    ITypeInfo *pTypeInfo;
    ITypeLib *pTypeLib;
    BSTR bstr;
    VALUE type;
    UINT i;
    UINT count;
    LCID    lcid = cWIN32OLE_lcid;
    HRESULT hr = pole->pDispatch->lpVtbl->GetTypeInfo(pole->pDispatch,
                                                      0, lcid, &pTypeInfo);
    if(FAILED(hr)) {
        ole_raise(hr, rb_eRuntimeError, "failed to GetTypeInfo");
    }
    hr = pTypeInfo->lpVtbl->GetDocumentation(pTypeInfo,
                                             -1,
                                             &bstr,
                                             NULL, NULL, NULL);
    type = WC2VSTR(bstr);
    hr = pTypeInfo->lpVtbl->GetContainingTypeLib(pTypeInfo, &pTypeLib, &i);
    OLE_RELEASE(pTypeInfo);
    if (FAILED(hr)) {
        ole_raise(hr, rb_eRuntimeError, "failed to GetContainingTypeLib");
    }
    count = pTypeLib->lpVtbl->GetTypeInfoCount(pTypeLib);
    for (i = 0; i < count; i++) {
        hr = pTypeLib->lpVtbl->GetDocumentation(pTypeLib, i,
                                                &bstr, NULL, NULL, NULL);
        if (SUCCEEDED(hr) && rb_str_cmp(WC2VSTR(bstr), type) == 0) {
            hr = pTypeLib->lpVtbl->GetTypeInfo(pTypeLib, i, &pTypeInfo);
            if (SUCCEEDED(hr)) {
                *ppti = pTypeInfo;
                break;
            }
        }
    }
    OLE_RELEASE(pTypeLib);
    return hr;
}

static VALUE
ole_methods(VALUE self, int mask)
{
    ITypeInfo *pTypeInfo;
    HRESULT hr;
    VALUE methods;
    struct oledata *pole;

    OLEData_Get_Struct(self, pole);
    methods = rb_ary_new();

    hr = typeinfo_from_ole(pole, &pTypeInfo);
    if(FAILED(hr))
        return methods;
    rb_ary_concat(methods, ole_methods_from_typeinfo(pTypeInfo, mask));
    OLE_RELEASE(pTypeInfo);
    return methods;
}

/*
 *  call-seq:
 *     WIN32OLE#ole_methods
 *
 *  Returns the array of WIN32OLE_METHOD object.
 *  The element is OLE method of WIN32OLE object.
 *
 *     excel = WIN32OLE.new('Excel.Application')
 *     methods = excel.ole_methods
 *
 */
static VALUE
fole_methods(VALUE self)
{
    return ole_methods( self, INVOKE_FUNC | INVOKE_PROPERTYGET | INVOKE_PROPERTYPUT | INVOKE_PROPERTYPUTREF);
}

/*
 *  call-seq:
 *     WIN32OLE#ole_get_methods
 *
 *  Returns the array of WIN32OLE_METHOD object .
 *  The element of the array is property (gettable) of WIN32OLE object.
 *
 *     excel = WIN32OLE.new('Excel.Application')
 *     properties = excel.ole_get_methods
 */
static VALUE
fole_get_methods(VALUE self)
{
    return ole_methods( self, INVOKE_PROPERTYGET);
}

/*
 *  call-seq:
 *     WIN32OLE#ole_put_methods
 *
 *  Returns the array of WIN32OLE_METHOD object .
 *  The element of the array is property (settable) of WIN32OLE object.
 *
 *     excel = WIN32OLE.new('Excel.Application')
 *     properties = excel.ole_put_methods
 */
static VALUE
fole_put_methods(VALUE self)
{
    return ole_methods( self, INVOKE_PROPERTYPUT|INVOKE_PROPERTYPUTREF);
}

/*
 *  call-seq:
 *     WIN32OLE#ole_func_methods
 *
 *  Returns the array of WIN32OLE_METHOD object .
 *  The element of the array is property (settable) of WIN32OLE object.
 *
 *     excel = WIN32OLE.new('Excel.Application')
 *     properties = excel.ole_func_methods
 *
 */
static VALUE
fole_func_methods(VALUE self)
{
    return ole_methods( self, INVOKE_FUNC);
}

/*
 *   call-seq:
 *      WIN32OLE#ole_type
 *
 *   Returns WIN32OLE_TYPE object.
 *
 *      excel = WIN32OLE.new('Excel.Application')
 *      tobj = excel.ole_type
 */
static VALUE
fole_type(VALUE self)
{
    ITypeInfo *pTypeInfo;
    HRESULT hr;
    struct oledata *pole;
    LCID  lcid = cWIN32OLE_lcid;
    VALUE type = Qnil;

    OLEData_Get_Struct(self, pole);

    hr = pole->pDispatch->lpVtbl->GetTypeInfo( pole->pDispatch, 0, lcid, &pTypeInfo );
    if(FAILED(hr)) {
        ole_raise(hr, rb_eRuntimeError, "failed to GetTypeInfo");
    }
    type = ole_type_from_itypeinfo(pTypeInfo);
    OLE_RELEASE(pTypeInfo);
    if (type == Qnil) {
        rb_raise(rb_eRuntimeError, "failed to create WIN32OLE_TYPE obj from ITypeInfo");
    }
    return type;
}

/*
 *  call-seq:
 *     WIN32OLE#ole_typelib -> The WIN32OLE_TYPELIB object
 *
 *  Returns the WIN32OLE_TYPELIB object. The object represents the
 *  type library which contains the WIN32OLE object.
 *
 *     excel = WIN32OLE.new('Excel.Application')
 *     tlib = excel.ole_typelib
 *     puts tlib.name  # -> 'Microsoft Excel 9.0 Object Library'
 */
static VALUE
fole_typelib(VALUE self)
{
    struct oledata *pole;
    HRESULT hr;
    ITypeInfo *pTypeInfo;
    LCID  lcid = cWIN32OLE_lcid;
    VALUE vtlib = Qnil;

    OLEData_Get_Struct(self, pole);
    hr = pole->pDispatch->lpVtbl->GetTypeInfo(pole->pDispatch,
                                              0, lcid, &pTypeInfo);
    if(FAILED(hr)) {
        ole_raise(hr, rb_eRuntimeError, "failed to GetTypeInfo");
    }
    vtlib = ole_typelib_from_itypeinfo(pTypeInfo);
    OLE_RELEASE(pTypeInfo);
    if (vtlib == Qnil) {
        rb_raise(rb_eRuntimeError, "failed to get type library info.");
    }
    return vtlib;
}

/*
 *  call-seq:
 *     WIN32OLE#ole_query_interface(iid) -> WIN32OLE object
 *
 *  Returns WIN32OLE object for a specific dispatch or dual
 *  interface specified by iid.
 *
 *      ie = WIN32OLE.new('InternetExplorer.Application')
 *      ie_web_app = ie.ole_query_interface('{0002DF05-0000-0000-C000-000000000046}') # => WIN32OLE object for dispinterface IWebBrowserApp
 */
static VALUE
fole_query_interface(VALUE self, VALUE str_iid)
{
    HRESULT hr;
    OLECHAR *pBuf;
    IID iid;
    struct oledata *pole;
    IDispatch *pDispatch;
    void *p;

    pBuf  = ole_vstr2wc(str_iid);
    hr = CLSIDFromString(pBuf, &iid);
    SysFreeString(pBuf);
    if(FAILED(hr)) {
        ole_raise(hr, eWIN32OLERuntimeError,
                  "invalid iid: `%s'",
                  StringValuePtr(str_iid));
    }

    OLEData_Get_Struct(self, pole);
    if(!pole->pDispatch) {
        rb_raise(rb_eRuntimeError, "failed to get dispatch interface");
    }

    hr = pole->pDispatch->lpVtbl->QueryInterface(pole->pDispatch, &iid,
                                                 &p);
    if(FAILED(hr)) {
        ole_raise(hr, eWIN32OLERuntimeError,
                  "failed to get interface `%s'",
                  StringValuePtr(str_iid));
    }

    pDispatch = p;
    return create_win32ole_object(cWIN32OLE, pDispatch, 0, 0);
}

/*
 *  call-seq:
 *     WIN32OLE#ole_respond_to?(method) -> true or false
 *
 *  Returns true when OLE object has OLE method, otherwise returns false.
 *
 *      ie = WIN32OLE.new('InternetExplorer.Application')
 *      ie.ole_respond_to?("gohome") => true
 */
static VALUE
fole_respond_to(VALUE self, VALUE method)
{
    struct oledata *pole;
    BSTR wcmdname;
    DISPID DispID;
    HRESULT hr;
    if(!RB_TYPE_P(method, T_STRING) && !RB_TYPE_P(method, T_SYMBOL)) {
	rb_raise(rb_eTypeError, "wrong argument type (expected String or Symbol)");
    }
    if (RB_TYPE_P(method, T_SYMBOL)) {
	method = rb_sym_to_s(method);
    }
    OLEData_Get_Struct(self, pole);
    wcmdname = ole_vstr2wc(method);
    hr = pole->pDispatch->lpVtbl->GetIDsOfNames( pole->pDispatch, &IID_NULL,
	    &wcmdname, 1, cWIN32OLE_lcid, &DispID);
    SysFreeString(wcmdname);
    return SUCCEEDED(hr) ? Qtrue : Qfalse;
}

HRESULT
ole_docinfo_from_type(ITypeInfo *pTypeInfo, BSTR *name, BSTR *helpstr, DWORD *helpcontext, BSTR *helpfile)
{
    HRESULT hr;
    ITypeLib *pTypeLib;
    UINT i;

    hr = pTypeInfo->lpVtbl->GetContainingTypeLib(pTypeInfo, &pTypeLib, &i);
    if (FAILED(hr)) {
        return hr;
    }

    hr = pTypeLib->lpVtbl->GetDocumentation(pTypeLib, i,
                                            name, helpstr,
                                            helpcontext, helpfile);
    if (FAILED(hr)) {
        OLE_RELEASE(pTypeLib);
        return hr;
    }
    OLE_RELEASE(pTypeLib);
    return hr;
}

static VALUE
ole_usertype2val(ITypeInfo *pTypeInfo, TYPEDESC *pTypeDesc, VALUE typedetails)
{
    HRESULT hr;
    BSTR bstr;
    ITypeInfo *pRefTypeInfo;
    VALUE type = Qnil;

    hr = pTypeInfo->lpVtbl->GetRefTypeInfo(pTypeInfo,
                                           V_UNION1(pTypeDesc, hreftype),
                                           &pRefTypeInfo);
    if(FAILED(hr))
        return Qnil;
    hr = ole_docinfo_from_type(pRefTypeInfo, &bstr, NULL, NULL, NULL);
    if(FAILED(hr)) {
        OLE_RELEASE(pRefTypeInfo);
        return Qnil;
    }
    OLE_RELEASE(pRefTypeInfo);
    type = WC2VSTR(bstr);
    if(typedetails != Qnil)
        rb_ary_push(typedetails, type);
    return type;
}

static VALUE
ole_ptrtype2val(ITypeInfo *pTypeInfo, TYPEDESC *pTypeDesc, VALUE typedetails)
{
    TYPEDESC *p = pTypeDesc;
    VALUE type = rb_str_new2("");

    if (p->vt == VT_PTR || p->vt == VT_SAFEARRAY) {
        p = V_UNION1(p, lptdesc);
        type = ole_typedesc2val(pTypeInfo, p, typedetails);
    }
    return type;
}

VALUE
ole_typedesc2val(ITypeInfo *pTypeInfo, TYPEDESC *pTypeDesc, VALUE typedetails)
{
    VALUE str;
    VALUE typestr = Qnil;
    switch(pTypeDesc->vt) {
    case VT_I2:
        typestr = rb_str_new2("I2");
        break;
    case VT_I4:
        typestr = rb_str_new2("I4");
        break;
    case VT_R4:
        typestr = rb_str_new2("R4");
        break;
    case VT_R8:
        typestr = rb_str_new2("R8");
        break;
    case VT_CY:
        typestr = rb_str_new2("CY");
        break;
    case VT_DATE:
        typestr = rb_str_new2("DATE");
        break;
    case VT_BSTR:
        typestr = rb_str_new2("BSTR");
        break;
    case VT_BOOL:
        typestr = rb_str_new2("BOOL");
        break;
    case VT_VARIANT:
        typestr = rb_str_new2("VARIANT");
        break;
    case VT_DECIMAL:
        typestr = rb_str_new2("DECIMAL");
        break;
    case VT_I1:
        typestr = rb_str_new2("I1");
        break;
    case VT_UI1:
        typestr = rb_str_new2("UI1");
        break;
    case VT_UI2:
        typestr = rb_str_new2("UI2");
        break;
    case VT_UI4:
        typestr = rb_str_new2("UI4");
        break;
#if (_MSC_VER >= 1300) || defined(__CYGWIN__) || defined(__MINGW32__)
    case VT_I8:
        typestr = rb_str_new2("I8");
        break;
    case VT_UI8:
        typestr = rb_str_new2("UI8");
        break;
#endif
    case VT_INT:
        typestr = rb_str_new2("INT");
        break;
    case VT_UINT:
        typestr = rb_str_new2("UINT");
        break;
    case VT_VOID:
        typestr = rb_str_new2("VOID");
        break;
    case VT_HRESULT:
        typestr = rb_str_new2("HRESULT");
        break;
    case VT_PTR:
        typestr = rb_str_new2("PTR");
        if(typedetails != Qnil)
            rb_ary_push(typedetails, typestr);
        return ole_ptrtype2val(pTypeInfo, pTypeDesc, typedetails);
    case VT_SAFEARRAY:
        typestr = rb_str_new2("SAFEARRAY");
        if(typedetails != Qnil)
            rb_ary_push(typedetails, typestr);
        return ole_ptrtype2val(pTypeInfo, pTypeDesc, typedetails);
    case VT_CARRAY:
        typestr = rb_str_new2("CARRAY");
        break;
    case VT_USERDEFINED:
        typestr = rb_str_new2("USERDEFINED");
        if (typedetails != Qnil)
            rb_ary_push(typedetails, typestr);
        str = ole_usertype2val(pTypeInfo, pTypeDesc, typedetails);
        if (str != Qnil) {
            return str;
        }
        return typestr;
    case VT_UNKNOWN:
        typestr = rb_str_new2("UNKNOWN");
        break;
    case VT_DISPATCH:
        typestr = rb_str_new2("DISPATCH");
        break;
    case VT_ERROR:
        typestr = rb_str_new2("ERROR");
        break;
    case VT_LPWSTR:
        typestr = rb_str_new2("LPWSTR");
        break;
    case VT_LPSTR:
        typestr = rb_str_new2("LPSTR");
        break;
    case VT_RECORD:
        typestr = rb_str_new2("RECORD");
        break;
    default:
        typestr = rb_str_new2("Unknown Type ");
        rb_str_concat(typestr, rb_fix2str(INT2FIX(pTypeDesc->vt), 10));
        break;
    }
    if (typedetails != Qnil)
        rb_ary_push(typedetails, typestr);
    return typestr;
}

/*
 *   call-seq:
 *      WIN32OLE#ole_method_help(method)
 *
 *   Returns WIN32OLE_METHOD object corresponding with method
 *   specified by 1st argument.
 *
 *      excel = WIN32OLE.new('Excel.Application')
 *      method = excel.ole_method_help('Quit')
 *
 */
static VALUE
fole_method_help(VALUE self, VALUE cmdname)
{
    ITypeInfo *pTypeInfo;
    HRESULT hr;
    struct oledata *pole;
    VALUE obj;

    SafeStringValue(cmdname);
    OLEData_Get_Struct(self, pole);
    hr = typeinfo_from_ole(pole, &pTypeInfo);
    if(FAILED(hr))
        ole_raise(hr, rb_eRuntimeError, "failed to get ITypeInfo");

    obj = create_win32ole_method(pTypeInfo, cmdname);

    OLE_RELEASE(pTypeInfo);
    if (obj == Qnil)
        rb_raise(eWIN32OLERuntimeError, "not found %s",
                 StringValuePtr(cmdname));
    return obj;
}

/*
 *  call-seq:
 *     WIN32OLE#ole_activex_initialize() -> Qnil
 *
 *  Initialize WIN32OLE object(ActiveX Control) by calling
 *  IPersistMemory::InitNew.
 *
 *  Before calling OLE method, some kind of the ActiveX controls
 *  created with MFC should be initialized by calling
 *  IPersistXXX::InitNew.
 *
 *  If and only if you received the exception "HRESULT error code:
 *  0x8000ffff catastrophic failure", try this method before
 *  invoking any ole_method.
 *
 *     obj = WIN32OLE.new("ProgID_or_GUID_of_ActiveX_Control")
 *     obj.ole_activex_initialize
 *     obj.method(...)
 *
 */
static VALUE
fole_activex_initialize(VALUE self)
{
    struct oledata *pole;
    IPersistMemory *pPersistMemory;
    void *p;

    HRESULT hr = S_OK;

    OLEData_Get_Struct(self, pole);

    hr = pole->pDispatch->lpVtbl->QueryInterface(pole->pDispatch, &IID_IPersistMemory, &p);
    pPersistMemory = p;
    if (SUCCEEDED(hr)) {
        hr = pPersistMemory->lpVtbl->InitNew(pPersistMemory);
        OLE_RELEASE(pPersistMemory);
        if (SUCCEEDED(hr)) {
            return Qnil;
        }
    }

    if (FAILED(hr)) {
        ole_raise(hr, eWIN32OLERuntimeError, "fail to initialize ActiveX control");
    }

    return Qnil;
}

/*
 * Document-class: WIN32OLE_EVENT
 *
 *   <code>WIN32OLE_EVENT</code> objects controls OLE event.
 */

static IEventSinkVtbl vtEventSink;
static BOOL g_IsEventSinkVtblInitialized = FALSE;

void EVENTSINK_Destructor(PIEVENTSINKOBJ);

STDMETHODIMP
EVENTSINK_QueryInterface(
    PEVENTSINK pEV,
    REFIID     iid,
    LPVOID*    ppv
    ) {
    if (IsEqualIID(iid, &IID_IUnknown) ||
        IsEqualIID(iid, &IID_IDispatch) ||
        IsEqualIID(iid, &((PIEVENTSINKOBJ)pEV)->m_iid)) {
        *ppv = pEV;
    }
    else {
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    ((LPUNKNOWN)*ppv)->lpVtbl->AddRef((LPUNKNOWN)*ppv);
    return NOERROR;
}

STDMETHODIMP_(ULONG)
EVENTSINK_AddRef(
    PEVENTSINK pEV
    ){
    PIEVENTSINKOBJ pEVObj = (PIEVENTSINKOBJ)pEV;
    return ++pEVObj->m_cRef;
}

STDMETHODIMP_(ULONG) EVENTSINK_Release(
    PEVENTSINK pEV
    ) {
    PIEVENTSINKOBJ pEVObj = (PIEVENTSINKOBJ)pEV;
    --pEVObj->m_cRef;
    if(pEVObj->m_cRef != 0)
        return pEVObj->m_cRef;
    EVENTSINK_Destructor(pEVObj);
    return 0;
}

STDMETHODIMP EVENTSINK_GetTypeInfoCount(
    PEVENTSINK pEV,
    UINT *pct
    ) {
    *pct = 0;
    return NOERROR;
}

STDMETHODIMP EVENTSINK_GetTypeInfo(
    PEVENTSINK pEV,
    UINT info,
    LCID lcid,
    ITypeInfo **pInfo
    ) {
    *pInfo = NULL;
    return DISP_E_BADINDEX;
}

STDMETHODIMP EVENTSINK_GetIDsOfNames(
    PEVENTSINK pEventSink,
    REFIID riid,
    OLECHAR **szNames,
    UINT cNames,
    LCID lcid,
    DISPID *pDispID
    ) {
    ITypeInfo *pTypeInfo;
    PIEVENTSINKOBJ pEV = (PIEVENTSINKOBJ)pEventSink;
    pTypeInfo = pEV->pTypeInfo;
    if (pTypeInfo) {
	return pTypeInfo->lpVtbl->GetIDsOfNames(pTypeInfo, szNames, cNames, pDispID);
    }
    return DISP_E_UNKNOWNNAME;
}

static long
ole_search_event_at(VALUE ary, VALUE ev)
{
    VALUE event;
    VALUE event_name;
    long i, len;
    long ret = -1;
    len = RARRAY_LEN(ary);
    for(i = 0; i < len; i++) {
        event = rb_ary_entry(ary, i);
        event_name = rb_ary_entry(event, 1);
        if(NIL_P(event_name) && NIL_P(ev)) {
            ret = i;
            break;
        }
        else if (RB_TYPE_P(ev, T_STRING) &&
                 RB_TYPE_P(event_name, T_STRING) &&
                 rb_str_cmp(ev, event_name) == 0) {
            ret = i;
            break;
        }
    }
    return ret;
}

static VALUE
ole_search_event(VALUE ary, VALUE ev, BOOL  *is_default)
{
    VALUE event;
    VALUE def_event;
    VALUE event_name;
    int i, len;
    *is_default = FALSE;
    def_event = Qnil;
    len = RARRAY_LEN(ary);
    for(i = 0; i < len; i++) {
        event = rb_ary_entry(ary, i);
        event_name = rb_ary_entry(event, 1);
        if(NIL_P(event_name)) {
            *is_default = TRUE;
            def_event = event;
        }
        else if (rb_str_cmp(ev, event_name) == 0) {
            *is_default = FALSE;
            return event;
        }
    }
    return def_event;
}
static VALUE
ole_search_handler_method(VALUE handler, VALUE ev, BOOL *is_default_handler)
{
    VALUE mid;

    *is_default_handler = FALSE;
    mid = rb_to_id(rb_sprintf("on%"PRIsVALUE, ev));
    if (rb_respond_to(handler, mid)) {
	return mid;
    }
    mid = rb_intern("method_missing");
    if (rb_respond_to(handler, mid)) {
	*is_default_handler = TRUE;
	return mid;
    }
    return Qnil;
}

static void
ole_delete_event(VALUE ary, VALUE ev)
{
    long at = -1;
    at = ole_search_event_at(ary, ev);
    if (at >= 0) {
        rb_ary_delete_at(ary, at);
    }
}

static void
hash2ptr_dispparams(VALUE hash, ITypeInfo *pTypeInfo, DISPID dispid, DISPPARAMS *pdispparams)
{
    BSTR *bstrs;
    HRESULT hr;
    UINT len, i;
    VARIANT *pvar;
    VALUE val;
    VALUE key;
    len = 0;
    bstrs = ALLOCA_N(BSTR, pdispparams->cArgs + 1);
    hr = pTypeInfo->lpVtbl->GetNames(pTypeInfo, dispid,
                                     bstrs, pdispparams->cArgs + 1,
                                     &len);
    if (FAILED(hr))
	return;

    for (i = 0; i < len - 1; i++) {
	key = WC2VSTR(bstrs[i + 1]);
        val = rb_hash_aref(hash, INT2FIX(i));
	if (val == Qnil)
	    val = rb_hash_aref(hash, key);
	if (val == Qnil)
	    val = rb_hash_aref(hash, rb_str_intern(key));
        pvar = &pdispparams->rgvarg[pdispparams->cArgs-i-1];
        ole_val2ptr_variant(val, pvar);
    }
}

static VALUE
hash2result(VALUE hash)
{
    VALUE ret = Qnil;
    ret = rb_hash_aref(hash, rb_str_new2("return"));
    if (ret == Qnil)
	ret = rb_hash_aref(hash, rb_str_intern(rb_str_new2("return")));
    return ret;
}

static void
ary2ptr_dispparams(VALUE ary, DISPPARAMS *pdispparams)
{
    int i;
    VALUE v;
    VARIANT *pvar;
    for(i = 0; i < RARRAY_LEN(ary) && (unsigned int) i < pdispparams->cArgs; i++) {
        v = rb_ary_entry(ary, i);
        pvar = &pdispparams->rgvarg[pdispparams->cArgs-i-1];
        ole_val2ptr_variant(v, pvar);
    }
}

static VALUE
exec_callback(VALUE arg)
{
    VALUE *parg = (VALUE *)arg;
    VALUE handler = parg[0];
    VALUE mid = parg[1];
    VALUE args = parg[2];
    return rb_apply(handler, mid, args);
}

static VALUE
rescue_callback(VALUE arg)
{

    VALUE error;
    VALUE e = rb_errinfo();
    VALUE bt = rb_funcall(e, rb_intern("backtrace"), 0);
    VALUE msg = rb_funcall(e, rb_intern("message"), 0);
    bt = rb_ary_entry(bt, 0);
    error = rb_sprintf("%"PRIsVALUE": %"PRIsVALUE" (%s)\n", bt, msg, rb_obj_classname(e));
    rb_write_error(StringValuePtr(error));
    rb_backtrace();
    ruby_finalize();
    exit(-1);

    return Qnil;
}

STDMETHODIMP EVENTSINK_Invoke(
    PEVENTSINK pEventSink,
    DISPID dispid,
    REFIID riid,
    LCID lcid,
    WORD wFlags,
    DISPPARAMS *pdispparams,
    VARIANT *pvarResult,
    EXCEPINFO *pexcepinfo,
    UINT *puArgErr
    ) {

    HRESULT hr;
    BSTR bstr;
    unsigned int count;
    unsigned int i;
    ITypeInfo *pTypeInfo;
    VARIANT *pvar;
    VALUE ary, obj, event, args, outargv, ev, result;
    VALUE handler = Qnil;
    VALUE arg[3];
    VALUE mid;
    VALUE is_outarg = Qfalse;
    BOOL is_default_handler = FALSE;
    int state;

    PIEVENTSINKOBJ pEV = (PIEVENTSINKOBJ)pEventSink;
    pTypeInfo = pEV->pTypeInfo;
    obj = evs_entry(pEV->m_event_id);
    if (!rb_obj_is_kind_of(obj, cWIN32OLE_EVENT)) {
        return NOERROR;
    }

    ary = rb_ivar_get(obj, id_events);
    if (NIL_P(ary) || !RB_TYPE_P(ary, T_ARRAY)) {
        return NOERROR;
    }
    hr = pTypeInfo->lpVtbl->GetNames(pTypeInfo, dispid,
                                     &bstr, 1, &count);
    if (FAILED(hr)) {
        return NOERROR;
    }
    ev = WC2VSTR(bstr);
    event = ole_search_event(ary, ev, &is_default_handler);
    if (RB_TYPE_P(event, T_ARRAY)) {
	handler = rb_ary_entry(event, 0);
	mid = rb_intern("call");
	is_outarg = rb_ary_entry(event, 3);
    } else {
	handler = rb_ivar_get(obj, rb_intern("handler"));
	if (handler == Qnil) {
	    return NOERROR;
	}
	mid = ole_search_handler_method(handler, ev, &is_default_handler);
    }
    if (handler == Qnil || mid == Qnil) {
	return NOERROR;
    }

    args = rb_ary_new();
    if (is_default_handler) {
        rb_ary_push(args, ev);
    }

    /* make argument of event handler */
    for (i = 0; i < pdispparams->cArgs; ++i) {
        pvar = &pdispparams->rgvarg[pdispparams->cArgs-i-1];
        rb_ary_push(args, ole_variant2val(pvar));
    }
    outargv = Qnil;
    if (is_outarg == Qtrue) {
	outargv = rb_ary_new();
        rb_ary_push(args, outargv);
    }

    /*
     * if exception raised in event callback,
     * then you receive cfp consistency error.
     * to avoid this error we use begin rescue end.
     * and the exception raised then error message print
     * and exit ruby process by Win32OLE itself.
     */
    arg[0] = handler;
    arg[1] = mid;
    arg[2] = args;
    result = rb_protect(exec_callback, (VALUE)arg, &state);
    if (state != 0) {
	rescue_callback(Qnil);
    }
    if(RB_TYPE_P(result, T_HASH)) {
	hash2ptr_dispparams(result, pTypeInfo, dispid, pdispparams);
	result = hash2result(result);
    }else if (is_outarg == Qtrue && RB_TYPE_P(outargv, T_ARRAY)) {
	ary2ptr_dispparams(outargv, pdispparams);
    }

    if (pvarResult) {
	VariantInit(pvarResult);
        ole_val2variant(result, pvarResult);
    }

    return NOERROR;
}

PIEVENTSINKOBJ
EVENTSINK_Constructor() {
    PIEVENTSINKOBJ pEv;
    if (!g_IsEventSinkVtblInitialized) {
        vtEventSink.QueryInterface=EVENTSINK_QueryInterface;
        vtEventSink.AddRef = EVENTSINK_AddRef;
        vtEventSink.Release = EVENTSINK_Release;
        vtEventSink.Invoke = EVENTSINK_Invoke;
        vtEventSink.GetIDsOfNames = EVENTSINK_GetIDsOfNames;
        vtEventSink.GetTypeInfoCount = EVENTSINK_GetTypeInfoCount;
        vtEventSink.GetTypeInfo = EVENTSINK_GetTypeInfo;

        g_IsEventSinkVtblInitialized = TRUE;
    }
    pEv = ALLOC_N(IEVENTSINKOBJ, 1);
    if(pEv == NULL) return NULL;
    pEv->lpVtbl = &vtEventSink;
    pEv->m_cRef = 0;
    pEv->m_event_id = 0;
    pEv->pTypeInfo = NULL;
    return pEv;
}

void EVENTSINK_Destructor(
    PIEVENTSINKOBJ pEVObj
    ) {
    if(pEVObj != NULL) {
        OLE_RELEASE(pEVObj->pTypeInfo);
        free(pEVObj);
        pEVObj = NULL;
    }
}

static HRESULT
find_iid(VALUE ole, char *pitf, IID *piid, ITypeInfo **ppTypeInfo)
{
    HRESULT hr;
    IDispatch *pDispatch;
    ITypeInfo *pTypeInfo;
    ITypeLib *pTypeLib;
    TYPEATTR *pTypeAttr;
    HREFTYPE RefType;
    ITypeInfo *pImplTypeInfo;
    TYPEATTR *pImplTypeAttr;

    struct oledata *pole;
    unsigned int index;
    unsigned int count;
    int type;
    BSTR bstr;
    char *pstr;

    BOOL is_found = FALSE;
    LCID    lcid = cWIN32OLE_lcid;

    OLEData_Get_Struct(ole, pole);

    pDispatch = pole->pDispatch;

    hr = pDispatch->lpVtbl->GetTypeInfo(pDispatch, 0, lcid, &pTypeInfo);
    if (FAILED(hr))
        return hr;

    hr = pTypeInfo->lpVtbl->GetContainingTypeLib(pTypeInfo,
                                                 &pTypeLib,
                                                 &index);
    OLE_RELEASE(pTypeInfo);
    if (FAILED(hr))
        return hr;

    if (!pitf) {
        hr = pTypeLib->lpVtbl->GetTypeInfoOfGuid(pTypeLib,
                                                 piid,
                                                 ppTypeInfo);
        OLE_RELEASE(pTypeLib);
        return hr;
    }
    count = pTypeLib->lpVtbl->GetTypeInfoCount(pTypeLib);
    for (index = 0; index < count; index++) {
        hr = pTypeLib->lpVtbl->GetTypeInfo(pTypeLib,
                                           index,
                                           &pTypeInfo);
        if (FAILED(hr))
            break;
        hr = OLE_GET_TYPEATTR(pTypeInfo, &pTypeAttr);

        if(FAILED(hr)) {
            OLE_RELEASE(pTypeInfo);
            break;
        }
        if(pTypeAttr->typekind == TKIND_COCLASS) {
            for (type = 0; type < pTypeAttr->cImplTypes; type++) {
                hr = pTypeInfo->lpVtbl->GetRefTypeOfImplType(pTypeInfo,
                                                             type,
                                                             &RefType);
                if (FAILED(hr))
                    break;
                hr = pTypeInfo->lpVtbl->GetRefTypeInfo(pTypeInfo,
                                                       RefType,
                                                       &pImplTypeInfo);
                if (FAILED(hr))
                    break;

                hr = pImplTypeInfo->lpVtbl->GetDocumentation(pImplTypeInfo,
                                                             -1,
                                                             &bstr,
                                                             NULL, NULL, NULL);
                if (FAILED(hr)) {
                    OLE_RELEASE(pImplTypeInfo);
                    break;
                }
                pstr = ole_wc2mb(bstr);
                if (strcmp(pitf, pstr) == 0) {
                    hr = pImplTypeInfo->lpVtbl->GetTypeAttr(pImplTypeInfo,
                                                            &pImplTypeAttr);
                    if (SUCCEEDED(hr)) {
                        is_found = TRUE;
                        *piid = pImplTypeAttr->guid;
                        if (ppTypeInfo) {
                            *ppTypeInfo = pImplTypeInfo;
                            (*ppTypeInfo)->lpVtbl->AddRef((*ppTypeInfo));
                        }
                        pImplTypeInfo->lpVtbl->ReleaseTypeAttr(pImplTypeInfo,
                                                               pImplTypeAttr);
                    }
                }
                free(pstr);
                OLE_RELEASE(pImplTypeInfo);
                if (is_found || FAILED(hr))
                    break;
            }
        }

        OLE_RELEASE_TYPEATTR(pTypeInfo, pTypeAttr);
        OLE_RELEASE(pTypeInfo);
        if (is_found || FAILED(hr))
            break;
    }
    OLE_RELEASE(pTypeLib);
    if(!is_found)
        return E_NOINTERFACE;
    return hr;
}

static HRESULT
find_coclass(
    ITypeInfo *pTypeInfo,
    TYPEATTR *pTypeAttr,
    ITypeInfo **pCOTypeInfo,
    TYPEATTR **pCOTypeAttr)
{
    HRESULT hr = E_NOINTERFACE;
    ITypeLib *pTypeLib;
    int count;
    BOOL found = FALSE;
    ITypeInfo *pTypeInfo2;
    TYPEATTR *pTypeAttr2;
    int flags;
    int i,j;
    HREFTYPE href;
    ITypeInfo *pRefTypeInfo;
    TYPEATTR *pRefTypeAttr;

    hr = pTypeInfo->lpVtbl->GetContainingTypeLib(pTypeInfo, &pTypeLib, NULL);
    if (FAILED(hr)) {
	return hr;
    }
    count = pTypeLib->lpVtbl->GetTypeInfoCount(pTypeLib);
    for (i = 0; i < count && !found; i++) {
	hr = pTypeLib->lpVtbl->GetTypeInfo(pTypeLib, i, &pTypeInfo2);
	if (FAILED(hr))
	    continue;
	hr = OLE_GET_TYPEATTR(pTypeInfo2, &pTypeAttr2);
	if (FAILED(hr)) {
	    OLE_RELEASE(pTypeInfo2);
	    continue;
	}
	if (pTypeAttr2->typekind != TKIND_COCLASS) {
	    OLE_RELEASE_TYPEATTR(pTypeInfo2, pTypeAttr2);
	    OLE_RELEASE(pTypeInfo2);
	    continue;
	}
	for (j = 0; j < pTypeAttr2->cImplTypes && !found; j++) {
	    hr = pTypeInfo2->lpVtbl->GetImplTypeFlags(pTypeInfo2, j, &flags);
	    if (FAILED(hr))
		continue;
	    if (!(flags & IMPLTYPEFLAG_FDEFAULT))
		continue;
	    hr = pTypeInfo2->lpVtbl->GetRefTypeOfImplType(pTypeInfo2, j, &href);
	    if (FAILED(hr))
		continue;
	    hr = pTypeInfo2->lpVtbl->GetRefTypeInfo(pTypeInfo2, href, &pRefTypeInfo);
	    if (FAILED(hr))
		continue;
	    hr = OLE_GET_TYPEATTR(pRefTypeInfo, &pRefTypeAttr);
	    if (FAILED(hr))  {
		OLE_RELEASE(pRefTypeInfo);
		continue;
	    }
	    if (IsEqualGUID(&(pTypeAttr->guid), &(pRefTypeAttr->guid))) {
		found = TRUE;
	    }
	}
	if (!found) {
	    OLE_RELEASE_TYPEATTR(pTypeInfo2, pTypeAttr2);
	    OLE_RELEASE(pTypeInfo2);
	}
    }
    OLE_RELEASE(pTypeLib);
    if (found) {
	*pCOTypeInfo = pTypeInfo2;
	*pCOTypeAttr = pTypeAttr2;
	hr = S_OK;
    } else {
	hr = E_NOINTERFACE;
    }
    return hr;
}

static HRESULT
find_default_source_from_typeinfo(
    ITypeInfo *pTypeInfo,
    TYPEATTR *pTypeAttr,
    ITypeInfo **ppTypeInfo)
{
    int i = 0;
    HRESULT hr = E_NOINTERFACE;
    int flags;
    HREFTYPE hRefType;
    /* Enumerate all implemented types of the COCLASS */
    for (i = 0; i < pTypeAttr->cImplTypes; i++) {
        hr = pTypeInfo->lpVtbl->GetImplTypeFlags(pTypeInfo, i, &flags);
        if (FAILED(hr))
            continue;

        /*
           looking for the [default] [source]
           we just hope that it is a dispinterface :-)
        */
        if ((flags & IMPLTYPEFLAG_FDEFAULT) &&
            (flags & IMPLTYPEFLAG_FSOURCE)) {

            hr = pTypeInfo->lpVtbl->GetRefTypeOfImplType(pTypeInfo,
                                                         i, &hRefType);
            if (FAILED(hr))
                continue;
            hr = pTypeInfo->lpVtbl->GetRefTypeInfo(pTypeInfo,
                                                   hRefType, ppTypeInfo);
            if (SUCCEEDED(hr))
                break;
        }
    }
    return hr;
}

static HRESULT
find_default_source(VALUE ole, IID *piid, ITypeInfo **ppTypeInfo)
{
    HRESULT hr;
    IProvideClassInfo2 *pProvideClassInfo2;
    IProvideClassInfo *pProvideClassInfo;
    void *p;

    IDispatch *pDispatch;
    ITypeInfo *pTypeInfo;
    ITypeInfo *pTypeInfo2 = NULL;
    TYPEATTR *pTypeAttr;
    TYPEATTR *pTypeAttr2 = NULL;

    struct oledata *pole;

    OLEData_Get_Struct(ole, pole);
    pDispatch = pole->pDispatch;
    hr = pDispatch->lpVtbl->QueryInterface(pDispatch,
                                           &IID_IProvideClassInfo2,
                                           &p);
    if (SUCCEEDED(hr)) {
        pProvideClassInfo2 = p;
        hr = pProvideClassInfo2->lpVtbl->GetGUID(pProvideClassInfo2,
                                                 GUIDKIND_DEFAULT_SOURCE_DISP_IID,
                                                 piid);
        OLE_RELEASE(pProvideClassInfo2);
        if (SUCCEEDED(hr)) {
            hr = find_iid(ole, NULL, piid, ppTypeInfo);
        }
    }
    if (SUCCEEDED(hr)) {
        return hr;
    }
    hr = pDispatch->lpVtbl->QueryInterface(pDispatch,
                                           &IID_IProvideClassInfo,
					   &p);
    if (SUCCEEDED(hr)) {
        pProvideClassInfo = p;
        hr = pProvideClassInfo->lpVtbl->GetClassInfo(pProvideClassInfo,
                                                     &pTypeInfo);
        OLE_RELEASE(pProvideClassInfo);
    }
    if (FAILED(hr)) {
        hr = pDispatch->lpVtbl->GetTypeInfo(pDispatch, 0, cWIN32OLE_lcid, &pTypeInfo );
    }
    if (FAILED(hr))
        return hr;
    hr = OLE_GET_TYPEATTR(pTypeInfo, &pTypeAttr);
    if (FAILED(hr)) {
        OLE_RELEASE(pTypeInfo);
        return hr;
    }

    *ppTypeInfo = 0;
    hr = find_default_source_from_typeinfo(pTypeInfo, pTypeAttr, ppTypeInfo);
    if (!*ppTypeInfo) {
	hr = find_coclass(pTypeInfo, pTypeAttr, &pTypeInfo2, &pTypeAttr2);
	if (SUCCEEDED(hr)) {
	    hr = find_default_source_from_typeinfo(pTypeInfo2, pTypeAttr2, ppTypeInfo);
	    OLE_RELEASE_TYPEATTR(pTypeInfo2, pTypeAttr2);
	    OLE_RELEASE(pTypeInfo2);
	}
    }
    OLE_RELEASE_TYPEATTR(pTypeInfo, pTypeAttr);
    OLE_RELEASE(pTypeInfo);
    /* Now that would be a bad surprise, if we didn't find it, wouldn't it? */
    if (!*ppTypeInfo) {
        if (SUCCEEDED(hr))
            hr = E_UNEXPECTED;
        return hr;
    }

    /* Determine IID of default source interface */
    hr = (*ppTypeInfo)->lpVtbl->GetTypeAttr(*ppTypeInfo, &pTypeAttr);
    if (SUCCEEDED(hr)) {
        *piid = pTypeAttr->guid;
        (*ppTypeInfo)->lpVtbl->ReleaseTypeAttr(*ppTypeInfo, pTypeAttr);
    }
    else
        OLE_RELEASE(*ppTypeInfo);

    return hr;

}

static void
ole_event_free(struct oleeventdata *poleev)
{
    if (poleev->pConnectionPoint) {
        poleev->pConnectionPoint->lpVtbl->Unadvise(poleev->pConnectionPoint, poleev->dwCookie);
        OLE_RELEASE(poleev->pConnectionPoint);
        poleev->pConnectionPoint = NULL;
    }
    free(poleev);
}

static VALUE
fev_s_allocate(VALUE klass)
{
    VALUE obj;
    struct oleeventdata *poleev;
    obj = Data_Make_Struct(klass,struct oleeventdata,0,ole_event_free,poleev);
    poleev->dwCookie = 0;
    poleev->pConnectionPoint = NULL;
    poleev->event_id = 0;
    return obj;
}

static VALUE
ev_advise(int argc, VALUE *argv, VALUE self)
{

    VALUE ole, itf;
    struct oledata *pole;
    char *pitf;
    HRESULT hr;
    IID iid;
    ITypeInfo *pTypeInfo = 0;
    IDispatch *pDispatch;
    IConnectionPointContainer *pContainer;
    IConnectionPoint *pConnectionPoint;
    IEVENTSINKOBJ *pIEV;
    DWORD dwCookie;
    struct oleeventdata *poleev;
    void *p;

    rb_scan_args(argc, argv, "11", &ole, &itf);

    if (!rb_obj_is_kind_of(ole, cWIN32OLE)) {
        rb_raise(rb_eTypeError, "1st parameter must be WIN32OLE object");
    }

    if(!RB_TYPE_P(itf, T_NIL)) {
        if (rb_safe_level() > 0 && OBJ_TAINTED(itf)) {
            rb_raise(rb_eSecurityError, "Insecure Event Creation - %s",
                     StringValuePtr(itf));
        }
        SafeStringValue(itf);
        pitf = StringValuePtr(itf);
        hr = find_iid(ole, pitf, &iid, &pTypeInfo);
    }
    else {
        hr = find_default_source(ole, &iid, &pTypeInfo);
    }
    if (FAILED(hr)) {
        ole_raise(hr, rb_eRuntimeError, "interface not found");
    }

    OLEData_Get_Struct(ole, pole);
    pDispatch = pole->pDispatch;
    hr = pDispatch->lpVtbl->QueryInterface(pDispatch,
                                           &IID_IConnectionPointContainer,
                                           &p);
    if (FAILED(hr)) {
        OLE_RELEASE(pTypeInfo);
        ole_raise(hr, rb_eRuntimeError,
                  "failed to query IConnectionPointContainer");
    }
    pContainer = p;

    hr = pContainer->lpVtbl->FindConnectionPoint(pContainer,
                                                 &iid,
                                                 &pConnectionPoint);
    OLE_RELEASE(pContainer);
    if (FAILED(hr)) {
        OLE_RELEASE(pTypeInfo);
        ole_raise(hr, rb_eRuntimeError, "failed to query IConnectionPoint");
    }
    pIEV = EVENTSINK_Constructor();
    pIEV->m_iid = iid;
    hr = pConnectionPoint->lpVtbl->Advise(pConnectionPoint,
                                          (IUnknown*)pIEV,
                                          &dwCookie);
    if (FAILED(hr)) {
        ole_raise(hr, rb_eRuntimeError, "Advise Error");
    }

    Data_Get_Struct(self, struct oleeventdata, poleev);
    pIEV->m_event_id
        = NUM2INT(evs_length());
    pIEV->pTypeInfo = pTypeInfo;
    poleev->dwCookie = dwCookie;
    poleev->pConnectionPoint = pConnectionPoint;
    poleev->event_id = pIEV->m_event_id;

    return self;
}

/*
 *  call-seq:
 *     WIN32OLE_EVENT.new(ole, event) #=> WIN32OLE_EVENT object.
 *
 *  Returns OLE event object.
 *  The first argument specifies WIN32OLE object.
 *  The second argument specifies OLE event name.
 *     ie = WIN32OLE.new('InternetExplorer.Application')
 *     ev = WIN32OLE_EVENT.new(ie, 'DWebBrowserEvents')
 */
static VALUE
fev_initialize(int argc, VALUE *argv, VALUE self)
{
    ev_advise(argc, argv, self);
    evs_push(self);
    rb_ivar_set(self, id_events, rb_ary_new());
    fev_set_handler(self, Qnil);
    return self;
}

/*
 *  call-seq:
 *     WIN32OLE_EVENT.message_loop
 *
 *  Translates and dispatches Windows message.
 */
static VALUE
fev_s_msg_loop(VALUE klass)
{
    ole_msg_loop();
    return Qnil;
}


static void
add_event_call_back(VALUE obj, VALUE event, VALUE data)
{
    VALUE events = rb_ivar_get(obj, id_events);
    if (NIL_P(events) || !RB_TYPE_P(events, T_ARRAY)) {
        events = rb_ary_new();
        rb_ivar_set(obj, id_events, events);
    }
    ole_delete_event(events, event);
    rb_ary_push(events, data);
}

static VALUE
ev_on_event(int argc, VALUE *argv, VALUE self, VALUE is_ary_arg)
{
    struct oleeventdata *poleev;
    VALUE event, args, data;
    Data_Get_Struct(self, struct oleeventdata, poleev);
    if (poleev->pConnectionPoint == NULL) {
        rb_raise(eWIN32OLERuntimeError, "IConnectionPoint not found. You must call advise at first.");
    }
    rb_scan_args(argc, argv, "01*", &event, &args);
    if(!NIL_P(event)) {
	if(!RB_TYPE_P(event, T_STRING) && !RB_TYPE_P(event, T_SYMBOL)) {
	    rb_raise(rb_eTypeError, "wrong argument type (expected String or Symbol)");
	}
	if (RB_TYPE_P(event, T_SYMBOL)) {
	    event = rb_sym_to_s(event);
	}
    }
    data = rb_ary_new3(4, rb_block_proc(), event, args, is_ary_arg);
    add_event_call_back(self, event, data);
    return Qnil;
}

/*
 *  call-seq:
 *     WIN32OLE_EVENT#on_event([event]){...}
 *
 *  Defines the callback event.
 *  If argument is omitted, this method defines the callback of all events.
 *  If you want to modify reference argument in callback, return hash in
 *  callback. If you want to return value to OLE server as result of callback
 *  use `return' or :return.
 *
 *    ie = WIN32OLE.new('InternetExplorer.Application')
 *    ev = WIN32OLE_EVENT.new(ie)
 *    ev.on_event("NavigateComplete") {|url| puts url}
 *    ev.on_event() {|ev, *args| puts "#{ev} fired"}
 *
 *    ev.on_event("BeforeNavigate2") {|*args|
 *      ...
 *      # set true to BeforeNavigate reference argument `Cancel'.
 *      # Cancel is 7-th argument of BeforeNavigate,
 *      # so you can use 6 as key of hash instead of 'Cancel'.
 *      # The argument is counted from 0.
 *      # The hash key of 0 means first argument.)
 *      {:Cancel => true}  # or {'Cancel' => true} or {6 => true}
 *    }
 *
 *    ev.on_event(...) {|*args|
 *      {:return => 1, :xxx => yyy}
 *    }
 */
static VALUE
fev_on_event(int argc, VALUE *argv, VALUE self)
{
    return ev_on_event(argc, argv, self, Qfalse);
}

/*
 *  call-seq:
 *     WIN32OLE_EVENT#on_event_with_outargs([event]){...}
 *
 *  Defines the callback of event.
 *  If you want modify argument in callback,
 *  you could use this method instead of WIN32OLE_EVENT#on_event.
 *
 *    ie = WIN32OLE.new('InternetExplorer.Application')
 *    ev = WIN32OLE_EVENT.new(ie)
 *    ev.on_event_with_outargs('BeforeNavigate2') {|*args|
 *      args.last[6] = true
 *    }
 */
static VALUE
fev_on_event_with_outargs(int argc, VALUE *argv, VALUE self)
{
    return ev_on_event(argc, argv, self, Qtrue);
}

/*
 *  call-seq:
 *     WIN32OLE_EVENT#off_event([event])
 *
 *  removes the callback of event.
 *
 *    ie = WIN32OLE.new('InternetExplorer.Application')
 *    ev = WIN32OLE_EVENT.new(ie)
 *    ev.on_event('BeforeNavigate2') {|*args|
 *      args.last[6] = true
 *    }
 *      ...
 *    ev.off_event('BeforeNavigate2')
 *      ...
 */
static VALUE
fev_off_event(int argc, VALUE *argv, VALUE self)
{
    VALUE event = Qnil;
    VALUE events;

    rb_scan_args(argc, argv, "01", &event);
    if(!NIL_P(event)) {
	if(!RB_TYPE_P(event, T_STRING) && !RB_TYPE_P(event, T_SYMBOL)) {
	    rb_raise(rb_eTypeError, "wrong argument type (expected String or Symbol)");
	}
	if (RB_TYPE_P(event, T_SYMBOL)) {
	    event = rb_sym_to_s(event);
	}
    }
    events = rb_ivar_get(self, id_events);
    if (NIL_P(events)) {
	return Qnil;
    }
    ole_delete_event(events, event);
    return Qnil;
}

/*
 *  call-seq:
 *     WIN32OLE_EVENT#unadvise -> nil
 *
 *  disconnects OLE server. If this method called, then the WIN32OLE_EVENT object
 *  does not receive the OLE server event any more.
 *  This method is trial implementation.
 *
 *      ie = WIN32OLE.new('InternetExplorer.Application')
 *      ev = WIN32OLE_EVENT.new(ie)
 *      ev.on_event() {...}
 *         ...
 *      ev.unadvise
 *
 */
static VALUE
fev_unadvise(VALUE self)
{
    struct oleeventdata *poleev;
    Data_Get_Struct(self, struct oleeventdata, poleev);
    if (poleev->pConnectionPoint) {
        ole_msg_loop();
        evs_delete(poleev->event_id);
        poleev->pConnectionPoint->lpVtbl->Unadvise(poleev->pConnectionPoint, poleev->dwCookie);
        OLE_RELEASE(poleev->pConnectionPoint);
        poleev->pConnectionPoint = NULL;
    }
    return Qnil;
}

static VALUE
evs_push(VALUE ev)
{
    return rb_ary_push(ary_ole_event, ev);
}

static VALUE
evs_delete(long i)
{
    rb_ary_store(ary_ole_event, i, Qnil);
    return Qnil;
}

static VALUE
evs_entry(long i)
{
    return rb_ary_entry(ary_ole_event, i);
}

static VALUE
evs_length(void)
{
    return rb_funcall(ary_ole_event, rb_intern("length"), 0);
}

/*
 *  call-seq:
 *     WIN32OLE_EVENT#handler=
 *
 *  sets event handler object. If handler object has onXXX
 *  method according to XXX event, then onXXX method is called
 *  when XXX event occurs.
 *
 *  If handler object has method_missing and there is no
 *  method according to the event, then method_missing
 *  called and 1-st argument is event name.
 *
 *  If handler object has onXXX method and there is block
 *  defined by WIN32OLE_EVENT#on_event('XXX'){},
 *  then block is executed but handler object method is not called
 *  when XXX event occurs.
 *
 *      class Handler
 *        def onStatusTextChange(text)
 *          puts "StatusTextChanged"
 *        end
 *        def onPropertyChange(prop)
 *          puts "PropertyChanged"
 *        end
 *        def method_missing(ev, *arg)
 *          puts "other event #{ev}"
 *        end
 *      end
 *
 *      handler = Handler.new
 *      ie = WIN32OLE.new('InternetExplorer.Application')
 *      ev = WIN32OLE_EVENT.new(ie)
 *      ev.on_event("StatusTextChange") {|*args|
 *        puts "this block executed."
 *        puts "handler.onStatusTextChange method is not called."
 *      }
 *      ev.handler = handler
 *
 */
static VALUE
fev_set_handler(VALUE self, VALUE val)
{
    return rb_ivar_set(self, rb_intern("handler"), val);
}

/*
 *  call-seq:
 *     WIN32OLE_EVENT#handler
 *
 *  returns handler object.
 *
 */
static VALUE
fev_get_handler(VALUE self)
{
    return rb_ivar_get(self, rb_intern("handler"));
}

static HRESULT
typelib_from_val(VALUE obj, ITypeLib **pTypeLib)
{
    LCID lcid = cWIN32OLE_lcid;
    HRESULT hr;
    struct oledata *pole;
    unsigned int index;
    ITypeInfo *pTypeInfo;
    OLEData_Get_Struct(obj, pole);
    hr = pole->pDispatch->lpVtbl->GetTypeInfo(pole->pDispatch,
                                              0, lcid, &pTypeInfo);
    if (FAILED(hr)) {
        return hr;
    }
    hr = pTypeInfo->lpVtbl->GetContainingTypeLib(pTypeInfo, pTypeLib, &index);
    OLE_RELEASE(pTypeInfo);
    return hr;
}

static HRESULT
recordinfo_from_itypelib(ITypeLib *pTypeLib, VALUE name, IRecordInfo **ppri)
{

    unsigned int count;
    unsigned int i;
    ITypeInfo *pTypeInfo;
    HRESULT hr = OLE_E_LAST;
    BSTR bstr;

    count = pTypeLib->lpVtbl->GetTypeInfoCount(pTypeLib);
    for (i = 0; i < count; i++) {
        hr = pTypeLib->lpVtbl->GetDocumentation(pTypeLib, i,
                                                &bstr, NULL, NULL, NULL);
        if (FAILED(hr))
            continue;

        hr = pTypeLib->lpVtbl->GetTypeInfo(pTypeLib, i, &pTypeInfo);
        if (FAILED(hr))
            continue;

        if (rb_str_cmp(WC2VSTR(bstr), name) == 0) {
            hr = GetRecordInfoFromTypeInfo(pTypeInfo, ppri);
            OLE_RELEASE(pTypeInfo);
            return hr;
        }
        OLE_RELEASE(pTypeInfo);
    }
    hr = OLE_E_LAST;
    return hr;
}

static void
olerecord_set_ivar(VALUE obj, IRecordInfo *pri, void *prec)
{
    HRESULT hr;
    BSTR bstr;
    BSTR *bstrs;
    ULONG count = 0;
    ULONG i;
    VALUE fields;
    VALUE val;
    VARIANT var;
    void *pdata = NULL;
    struct olerecorddata *pvar;

    Data_Get_Struct(obj, struct olerecorddata, pvar);
    OLE_ADDREF(pri);
    OLE_RELEASE(pvar->pri);
    pvar->pri = pri;

    hr = pri->lpVtbl->GetName(pri, &bstr);
    if (SUCCEEDED(hr)) {
        rb_ivar_set(obj, rb_intern("typename"), WC2VSTR(bstr));
    }

    hr = pri->lpVtbl->GetFieldNames(pri, &count, NULL);
    if (FAILED(hr) || count == 0)
        return;
    bstrs = ALLOCA_N(BSTR, count);
    hr = pri->lpVtbl->GetFieldNames(pri, &count, bstrs);
    if (FAILED(hr)) {
        return;
    }

    fields = rb_hash_new();
    rb_ivar_set(obj, rb_intern("fields"), fields);
    for (i = 0; i < count; i++) {
        pdata = NULL;
        VariantInit(&var);
        val = Qnil;
        if (prec) {
            hr = pri->lpVtbl->GetFieldNoCopy(pri, prec, bstrs[i], &var, &pdata);
            if (SUCCEEDED(hr)) {
                val = ole_variant2val(&var);
            }
        }
        rb_hash_aset(fields, WC2VSTR(bstrs[i]), val);
    }
}

/*
 * Document-class: WIN32OLE_RECORD
 *
 *   <code>WIN32OLE_RECORD</code> objects represents VT_RECORD OLE variant.
 *   Win32OLE returns WIN32OLE_RECORD object if the result value of invoking
 *   OLE methods.
 *
 *   If COM server in VB.NET ComServer project is the following:
 *
 *     Imports System.Runtime.InteropServices
 *     Public Class ComClass
 *         Public Structure Book
 *             <MarshalAs(UnmanagedType.BStr)> _
 *             Public title As String
 *             Public cost As Integer
 *         End Structure
 *         Public Function getBook() As Book
 *             Dim book As New Book
 *             book.title = "The Ruby Book"
 *             book.cost = 20
 *             Return book
 *         End Function
 *     End Class
 *
 *   then, you can retrieve getBook return value from the following
 *   Ruby script:
 *
 *     require 'win32ole'
 *     obj = WIN32OLE.new('ComServer.ComClass')
 *     book = obj.getBook
 *     book.class # => WIN32OLE_RECORD
 *     book.title # => "The Ruby Book"
 *     book.cost  # => 20
 *
 */

static void
olerecord_free(struct olerecorddata *pvar) {
    OLE_FREE(pvar->pri);
    if (pvar->pdata) {
        free(pvar->pdata);
    }
    free(pvar);
}

static VALUE
folerecord_s_allocate(VALUE klass) {
    VALUE obj = Qnil;
    struct olerecorddata *pvar;
    obj = Data_Make_Struct(klass, struct olerecorddata, 0, olerecord_free, pvar);
    pvar->pri = NULL;
    pvar->pdata = NULL;
    return obj;
}

/*
 * call-seq:
 *    WIN32OLE_RECORD.new(typename, obj) -> WIN32OLE_RECORD object
 *
 * Returns WIN32OLE_RECORD object. The first argument is struct name (String
 * or Symbol).
 * The second parameter obj should be WIN32OLE object or WIN32OLE_TYPELIB object.
 * If COM server in VB.NET ComServer project is the following:
 *
 *   Imports System.Runtime.InteropServices
 *   Public Class ComClass
 *       Public Structure Book
 *           <MarshalAs(UnmanagedType.BStr)> _
 *           Public title As String
 *           Public cost As Integer
 *       End Structure
 *   End Class
 *
 * then, you can create WIN32OLE_RECORD object is as following:
 *
 *   require 'win32ole'
 *   obj = WIN32OLE.new('ComServer.ComClass')
 *   book1 = WIN32OLE_RECORD.new('Book', obj) # => WIN32OLE_RECORD object
 *   tlib = obj.ole_typelib
 *   book2 = WIN32OLE_RECORD.new('Book', tlib) # => WIN32OLE_RECORD object
 *
 */
static VALUE
folerecord_initialize(VALUE self, VALUE typename, VALUE oleobj) {
    HRESULT hr;
    ITypeLib *pTypeLib = NULL;
    IRecordInfo *pri = NULL;

    if (!RB_TYPE_P(typename, T_STRING) && !RB_TYPE_P(typename, T_SYMBOL)) {
        rb_raise(rb_eArgError, "1st argument should be String or Symbol");
    }
    if (RB_TYPE_P(typename, T_SYMBOL)) {
        typename = rb_sym_to_s(typename);
    }

    hr = S_OK;
    if(rb_obj_is_kind_of(oleobj, cWIN32OLE)) {
        hr = typelib_from_val(oleobj, &pTypeLib);
    } else if (rb_obj_is_kind_of(oleobj, cWIN32OLE_TYPELIB)) {
        pTypeLib = itypelib(oleobj);
        OLE_ADDREF(pTypeLib);
        if (pTypeLib) {
            hr = S_OK;
        } else {
            hr = E_FAIL;
        }
    } else {
        rb_raise(rb_eArgError, "2nd argument should be WIN32OLE object or WIN32OLE_TYPELIB object");
    }

    if (FAILED(hr)) {
        ole_raise(hr, eWIN32OLERuntimeError, "fail to query ITypeLib interface");
    }

    hr = recordinfo_from_itypelib(pTypeLib, typename, &pri);
    OLE_RELEASE(pTypeLib);
    if (FAILED(hr)) {
        ole_raise(hr, eWIN32OLERuntimeError, "fail to query IRecordInfo interface for `%s'", StringValuePtr(typename));
    }

    olerecord_set_ivar(self, pri, NULL);

    return self;
}

/*
 *  call-seq:
 *     WIN32OLE_RECORD#to_h #=> Ruby Hash object.
 *
 *  Returns Ruby Hash object which represents VT_RECORD variable.
 *  The keys of Hash object are member names of VT_RECORD OLE variable and
 *  the values of Hash object are values of VT_RECORD OLE variable.
 *
 *  If COM server in VB.NET ComServer project is the following:
 *
 *     Imports System.Runtime.InteropServices
 *     Public Class ComClass
 *         Public Structure Book
 *             <MarshalAs(UnmanagedType.BStr)> _
 *             Public title As String
 *             Public cost As Integer
 *         End Structure
 *         Public Function getBook() As Book
 *             Dim book As New Book
 *             book.title = "The Ruby Book"
 *             book.cost = 20
 *             Return book
 *         End Function
 *     End Class
 *
 *  then, the result of WIN32OLE_RECORD#to_h is the following:
 *
 *     require 'win32ole'
 *     obj = WIN32OLE.new('ComServer.ComClass')
 *     book = obj.getBook
 *     book.to_h # => {"title"=>"The Ruby Book", "cost"=>20}
 *
 */
static VALUE
folerecord_to_h(VALUE self)
{
    return rb_ivar_get(self, rb_intern("fields"));
}

/*
 *  call-seq:
 *     WIN32OLE_RECORD#typename #=> String object
 *
 *  Returns the type name of VT_RECORD OLE variable.
 *
 *  If COM server in VB.NET ComServer project is the following:
 *
 *     Imports System.Runtime.InteropServices
 *     Public Class ComClass
 *         Public Structure Book
 *             <MarshalAs(UnmanagedType.BStr)> _
 *             Public title As String
 *             Public cost As Integer
 *         End Structure
 *         Public Function getBook() As Book
 *             Dim book As New Book
 *             book.title = "The Ruby Book"
 *             book.cost = 20
 *             Return book
 *         End Function
 *     End Class
 *
 *  then, the result of WIN32OLE_RECORD#typename is the following:
 *
 *     require 'win32ole'
 *     obj = WIN32OLE.new('ComServer.ComClass')
 *     book = obj.getBook
 *     book.typename # => "Book"
 *
 */
static VALUE
folerecord_typename(VALUE self)
{
    return rb_ivar_get(self, rb_intern("typename"));
}

static VALUE
olerecord_ivar_get(VALUE self, VALUE name)
{
    VALUE fields;
    fields = rb_ivar_get(self, rb_intern("fields"));
    return rb_hash_fetch(fields, name);
}

static VALUE
olerecord_ivar_set(VALUE self, VALUE name, VALUE val)
{
    long len;
    char *p;
    VALUE fields;
    len  = RSTRING_LEN(name);
    p = RSTRING_PTR(name);
    if (p[len-1] == '=') {
        name = rb_str_subseq(name, 0, len-1);
    }
    fields = rb_ivar_get(self, rb_intern("fields"));
    rb_hash_fetch(fields, name);
    return rb_hash_aset(fields, name, val);
}

/*
 *  call-seq:
 *     WIN32OLE_RECORD#method_missing(name)
 *
 *  Returns value specified by the member name of VT_RECORD OLE variable.
 *  Or sets value specified by the member name of VT_RECORD OLE variable.
 *  If the member name is not correct, KeyError exception is raised.
 *
 *  If COM server in VB.NET ComServer project is the following:
 *
 *     Imports System.Runtime.InteropServices
 *     Public Class ComClass
 *         Public Structure Book
 *             <MarshalAs(UnmanagedType.BStr)> _
 *             Public title As String
 *             Public cost As Integer
 *         End Structure
 *     End Class
 *
 *  Then getting/setting value from Ruby is as the following:
 *
 *     obj = WIN32OLE.new('ComServer.ComClass')
 *     book = WIN32OLE_RECORD.new('Book', obj)
 *     book.title # => nil ( book.method_missing(:title) is invoked. )
 *     book.title = "Ruby" # ( book.method_missing(:title=, "Ruby") is invoked. )
 */
static VALUE
folerecord_method_missing(int argc, VALUE *argv, VALUE self)
{
    VALUE name;
    rb_check_arity(argc, 1, 2);
    name = rb_sym_to_s(argv[0]);

#if SIZEOF_SIZE_T > SIZEOF_LONG
    {
        size_t n = strlen(StringValueCStr(name));
        if (n >= LONG_MAX) {
            rb_raise(rb_eRuntimeError, "too long member name");
        }
    }
#endif

    if (argc == 1) {
        return olerecord_ivar_get(self, name);
    } else if (argc == 2) {
        return olerecord_ivar_set(self, name, argv[1]);
    }
    return Qnil;
}

/*
 *  call-seq:
 *     WIN32OLE_RECORD#ole_instance_variable_get(name)
 *
 *  Returns value specified by the member name of VT_RECORD OLE object.
 *  If the member name is not correct, KeyError exception is raised.
 *  If you can't access member variable of VT_RECORD OLE object directly,
 *  use this method.
 *
 *  If COM server in VB.NET ComServer project is the following:
 *
 *     Imports System.Runtime.InteropServices
 *     Public Class ComClass
 *         Public Structure ComObject
 *             Public object_id As Ineger
 *         End Structure
 *     End Class
 *
 *  and Ruby Object class has title attribute:
 *
 *  then accessing object_id of ComObject from Ruby is as the following:
 *
 *     srver = WIN32OLE.new('ComServer.ComClass')
 *     obj = WIN32OLE_RECORD.new('ComObject', server)
 *     # obj.object_id returns Ruby Object#object_id
 *     obj.ole_instance_variable_get(:object_id) # => nil
 *
 */
static VALUE
folerecord_ole_instance_variable_get(VALUE self, VALUE name)
{
    VALUE sname;
    if(!RB_TYPE_P(name, T_STRING) && !RB_TYPE_P(name, T_SYMBOL)) {
        rb_raise(rb_eTypeError, "wrong argument type (expected String or Symbol)");
    }
    sname = name;
    if (RB_TYPE_P(name, T_SYMBOL)) {
        sname = rb_sym_to_s(name);
    }
    return olerecord_ivar_get(self, sname);
}

/*
 *  call-seq:
 *     WIN32OLE_RECORD#ole_instance_variable_set(name, val)
 *
 *  Sets value specified by the member name of VT_RECORD OLE object.
 *  If the member name is not correct, KeyError exception is raised.
 *  If you can't set value of member of VT_RECORD OLE object directly,
 *  use this method.
 *
 *  If COM server in VB.NET ComServer project is the following:
 *
 *     Imports System.Runtime.InteropServices
 *     Public Class ComClass
 *         <MarshalAs(UnmanagedType.BStr)> _
 *         Public title As String
 *         Public cost As Integer
 *     End Class
 *
 *  then setting value of the `title' member is as following:
 *
 *     srver = WIN32OLE.new('ComServer.ComClass')
 *     obj = WIN32OLE_RECORD.new('Book', server)
 *     obj.ole_instance_variable_set(:title, "The Ruby Book")
 *
 */
static VALUE
folerecord_ole_instance_variable_set(VALUE self, VALUE name, VALUE val)
{
    VALUE sname;
    if(!RB_TYPE_P(name, T_STRING) && !RB_TYPE_P(name, T_SYMBOL)) {
        rb_raise(rb_eTypeError, "wrong argument type (expected String or Symbol)");
    }
    sname = name;
    if (RB_TYPE_P(name, T_SYMBOL)) {
        sname = rb_sym_to_s(name);
    }
    return olerecord_ivar_set(self, sname, val);
}

/*
 *  call-seq:
 *     WIN32OLE_RECORD#inspect -> String
 *
 *  Returns the OLE struct name and member name and the value of member
 *
 *  If COM server in VB.NET ComServer project is the following:
 *
 *     Imports System.Runtime.InteropServices
 *     Public Class ComClass
 *         <MarshalAs(UnmanagedType.BStr)> _
 *         Public title As String
 *         Public cost As Integer
 *     End Class
 *
 *  then
 *
 *     srver = WIN32OLE.new('ComServer.ComClass')
 *     obj = WIN32OLE_RECORD.new('Book', server)
 *     obj.inspect # => <WIN32OLE_RECORD(ComClass) {"title" => nil, "cost" => nil}>
 *
 */
static VALUE
folerecord_inspect(VALUE self)
{
    VALUE tname;
    VALUE field;
    tname = folerecord_typename(self);
    if (tname == Qnil) {
        tname = rb_inspect(tname);
    }
    field = rb_inspect(folerecord_to_h(self));
    return rb_sprintf("#<WIN32OLE_RECORD(%"PRIsVALUE") %"PRIsVALUE">",
                      tname,
                      field);
}

static void
init_enc2cp(void)
{
    enc2cp_table = st_init_numtable();
}

static void
free_enc2cp(void)
{
    st_free_table(enc2cp_table);
}

void
Init_win32ole(void)
{
    cWIN32OLE_lcid = LOCALE_SYSTEM_DEFAULT;
    g_ole_initialized_init();
    ary_ole_event = rb_ary_new();
    rb_gc_register_mark_object(ary_ole_event);
    id_events = rb_intern("events");

    com_vtbl.QueryInterface = QueryInterface;
    com_vtbl.AddRef = AddRef;
    com_vtbl.Release = Release;
    com_vtbl.GetTypeInfoCount = GetTypeInfoCount;
    com_vtbl.GetTypeInfo = GetTypeInfo;
    com_vtbl.GetIDsOfNames = GetIDsOfNames;
    com_vtbl.Invoke = Invoke;

    message_filter.QueryInterface = mf_QueryInterface;
    message_filter.AddRef = mf_AddRef;
    message_filter.Release = mf_Release;
    message_filter.HandleInComingCall = mf_HandleInComingCall;
    message_filter.RetryRejectedCall = mf_RetryRejectedCall;
    message_filter.MessagePending = mf_MessagePending;

    com_hash = Data_Wrap_Struct(rb_cData, rb_mark_hash, st_free_table, st_init_numtable());
    rb_gc_register_mark_object(com_hash);

    cWIN32OLE = rb_define_class("WIN32OLE", rb_cObject);

    rb_define_alloc_func(cWIN32OLE, fole_s_allocate);

    rb_define_method(cWIN32OLE, "initialize", fole_initialize, -1);

    rb_define_singleton_method(cWIN32OLE, "connect", fole_s_connect, -1);
    rb_define_singleton_method(cWIN32OLE, "const_load", fole_s_const_load, -1);

    rb_define_singleton_method(cWIN32OLE, "ole_free", fole_s_free, 1);
    rb_define_singleton_method(cWIN32OLE, "ole_reference_count", fole_s_reference_count, 1);
    rb_define_singleton_method(cWIN32OLE, "ole_show_help", fole_s_show_help, -1);
    rb_define_singleton_method(cWIN32OLE, "codepage", fole_s_get_code_page, 0);
    rb_define_singleton_method(cWIN32OLE, "codepage=", fole_s_set_code_page, 1);
    rb_define_singleton_method(cWIN32OLE, "locale", fole_s_get_locale, 0);
    rb_define_singleton_method(cWIN32OLE, "locale=", fole_s_set_locale, 1);
    rb_define_singleton_method(cWIN32OLE, "create_guid", fole_s_create_guid, 0);
    rb_define_singleton_method(cWIN32OLE, "ole_initialize", fole_s_ole_initialize, 0);
    rb_define_singleton_method(cWIN32OLE, "ole_uninitialize", fole_s_ole_uninitialize, 0);

    rb_define_method(cWIN32OLE, "invoke", fole_invoke, -1);
    rb_define_method(cWIN32OLE, "[]", fole_getproperty_with_bracket, -1);
    rb_define_method(cWIN32OLE, "_invoke", fole_invoke2, 3);
    rb_define_method(cWIN32OLE, "_getproperty", fole_getproperty2, 3);
    rb_define_method(cWIN32OLE, "_setproperty", fole_setproperty2, 3);

    /* support propput method that takes an argument */
    rb_define_method(cWIN32OLE, "[]=", fole_setproperty_with_bracket, -1);

    rb_define_method(cWIN32OLE, "ole_free", fole_free, 0);

    rb_define_method(cWIN32OLE, "each", fole_each, 0);
    rb_define_method(cWIN32OLE, "method_missing", fole_missing, -1);

    /* support setproperty method much like Perl ;-) */
    rb_define_method(cWIN32OLE, "setproperty", fole_setproperty, -1);

    rb_define_method(cWIN32OLE, "ole_methods", fole_methods, 0);
    rb_define_method(cWIN32OLE, "ole_get_methods", fole_get_methods, 0);
    rb_define_method(cWIN32OLE, "ole_put_methods", fole_put_methods, 0);
    rb_define_method(cWIN32OLE, "ole_func_methods", fole_func_methods, 0);

    rb_define_method(cWIN32OLE, "ole_method", fole_method_help, 1);
    rb_define_alias(cWIN32OLE, "ole_method_help", "ole_method");
    rb_define_method(cWIN32OLE, "ole_activex_initialize", fole_activex_initialize, 0);
    rb_define_method(cWIN32OLE, "ole_type", fole_type, 0);
    rb_define_alias(cWIN32OLE, "ole_obj_help", "ole_type");
    rb_define_method(cWIN32OLE, "ole_typelib", fole_typelib, 0);
    rb_define_method(cWIN32OLE, "ole_query_interface", fole_query_interface, 1);
    rb_define_method(cWIN32OLE, "ole_respond_to?", fole_respond_to, 1);

    /* Constants definition */

    /*
     * Version string of WIN32OLE.
     */
    rb_define_const(cWIN32OLE, "VERSION", rb_str_new2(WIN32OLE_VERSION));

    /*
     * After invoking OLE methods with reference arguments, you can access
     * the value of arguments by using ARGV.
     *
     * If the method of OLE(COM) server written by C#.NET is following:
     *
     *   void calcsum(int a, int b, out int c) {
     *       c = a + b;
     *   }
     *
     * then, the Ruby OLE(COM) client script to retrieve the value of
     * argument c after invoking calcsum method is following:
     *
     *   a = 10
     *   b = 20
     *   c = 0
     *   comserver.calcsum(a, b, c)
     *   p c # => 0
     *   p WIN32OLE::ARGV # => [10, 20, 30]
     *
     * You can use WIN32OLE_VARIANT object to retrieve the value of reference
     * arguments instead of refering WIN32OLE::ARGV.
     *
     */
    rb_define_const(cWIN32OLE, "ARGV", rb_ary_new());

    /*
     * 0: ANSI code page. See WIN32OLE.codepage and WIN32OLE.codepage=.
     */
    rb_define_const(cWIN32OLE, "CP_ACP", INT2FIX(CP_ACP));

    /*
     * 1: OEM code page. See WIN32OLE.codepage and WIN32OLE.codepage=.
     */
    rb_define_const(cWIN32OLE, "CP_OEMCP", INT2FIX(CP_OEMCP));

    /*
     * 2
     */
    rb_define_const(cWIN32OLE, "CP_MACCP", INT2FIX(CP_MACCP));

    /*
     * 3: current thread ANSI code page. See WIN32OLE.codepage and
     * WIN32OLE.codepage=.
     */
    rb_define_const(cWIN32OLE, "CP_THREAD_ACP", INT2FIX(CP_THREAD_ACP));

    /*
     * 42: symbol code page. See WIN32OLE.codepage and WIN32OLE.codepage=.
     */
    rb_define_const(cWIN32OLE, "CP_SYMBOL", INT2FIX(CP_SYMBOL));

    /*
     * 65000: UTF-7 code page. See WIN32OLE.codepage and WIN32OLE.codepage=.
     */
    rb_define_const(cWIN32OLE, "CP_UTF7", INT2FIX(CP_UTF7));

    /*
     * 65001: UTF-8 code page. See WIN32OLE.codepage and WIN32OLE.codepage=.
     */
    rb_define_const(cWIN32OLE, "CP_UTF8", INT2FIX(CP_UTF8));

    /*
     * 0x0800: default locale for the operating system. See WIN32OLE.locale
     * and WIN32OLE.locale=.
     */
    rb_define_const(cWIN32OLE, "LOCALE_SYSTEM_DEFAULT", INT2FIX(LOCALE_SYSTEM_DEFAULT));

    /*
     * 0x0400: default locale for the user or process. See WIN32OLE.locale
     * and WIN32OLE.locale=.
     */
    rb_define_const(cWIN32OLE, "LOCALE_USER_DEFAULT", INT2FIX(LOCALE_USER_DEFAULT));

    Init_win32ole_variant_m();
    Init_win32ole_typelib();
    Init_win32ole_type();
    Init_win32ole_variable();
    Init_win32ole_method();
    Init_win32ole_param();

    cWIN32OLE_EVENT = rb_define_class("WIN32OLE_EVENT", rb_cObject);
    rb_define_singleton_method(cWIN32OLE_EVENT, "message_loop", fev_s_msg_loop, 0);
    rb_define_alloc_func(cWIN32OLE_EVENT, fev_s_allocate);
    rb_define_method(cWIN32OLE_EVENT, "initialize", fev_initialize, -1);
    rb_define_method(cWIN32OLE_EVENT, "on_event", fev_on_event, -1);
    rb_define_method(cWIN32OLE_EVENT, "on_event_with_outargs", fev_on_event_with_outargs, -1);
    rb_define_method(cWIN32OLE_EVENT, "off_event", fev_off_event, -1);
    rb_define_method(cWIN32OLE_EVENT, "unadvise", fev_unadvise, 0);
    rb_define_method(cWIN32OLE_EVENT, "handler=", fev_set_handler, 1);
    rb_define_method(cWIN32OLE_EVENT, "handler", fev_get_handler, 0);

    Init_win32ole_variant();

    cWIN32OLE_RECORD = rb_define_class("WIN32OLE_RECORD", rb_cObject);
    rb_define_alloc_func(cWIN32OLE_RECORD, folerecord_s_allocate);
    rb_define_method(cWIN32OLE_RECORD, "initialize", folerecord_initialize, 2);
    rb_define_method(cWIN32OLE_RECORD, "to_h", folerecord_to_h, 0);
    rb_define_method(cWIN32OLE_RECORD, "typename", folerecord_typename, 0);
    rb_define_method(cWIN32OLE_RECORD, "method_missing", folerecord_method_missing, -1);
    rb_define_method(cWIN32OLE_RECORD, "ole_instance_variable_get", folerecord_ole_instance_variable_get, 1);
    rb_define_method(cWIN32OLE_RECORD, "ole_instance_variable_set", folerecord_ole_instance_variable_set, 2);
    rb_define_method(cWIN32OLE_RECORD, "inspect", folerecord_inspect, 0);

    Init_win32ole_error();

    init_enc2cp();
    atexit((void (*)(void))free_enc2cp);
    ole_init_cp();
}
