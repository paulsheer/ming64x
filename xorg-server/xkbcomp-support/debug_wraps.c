/* Wrapper functions for xkbcomp linking.
   Uses linker --wrap to intercept libX11/libxkbfile calls. */
typedef int Bool;
typedef int Status;
#define True 1
#define False 0
typedef struct _XkbFile XkbFile;
typedef struct _XkbFileInfo XkbFileInfo;
typedef struct _XkbDesc * XkbDescPtr;
typedef struct _InterpDef InterpDef;
typedef struct _ExprDef ExprDef;

Bool __real_CompileCompatMap(const XkbFile *file, XkbFileInfo *result, unsigned merge, void **unboundLEDs);
Bool __real_CompileSymbols(XkbFile *file, XkbFileInfo *result, unsigned merge);
Bool __real_BindIndicators(XkbFileInfo *result, Bool save, void *unbound, void *pCollide);
Status __real_XkbAllocCompatMap(XkbDescPtr xkb, unsigned which, int numSI);
int __real_yylex(void);
InterpDef *__real_InterpCreate(const char *sym_str, ExprDef *match);

Bool __wrap_CompileCompatMap(const XkbFile *file, XkbFileInfo *result, unsigned merge, void **unboundLEDs)
{
    return __real_CompileCompatMap(file, result, merge, unboundLEDs);
}

Bool __wrap_CompileSymbols(XkbFile *file, XkbFileInfo *result, unsigned merge)
{
    return __real_CompileSymbols(file, result, merge);
}

Bool __wrap_BindIndicators(XkbFileInfo *result, Bool save, void *unbound, void *pCollide)
{
    return __real_BindIndicators(result, save, unbound, pCollide);
}

Status __wrap_XkbAllocCompatMap(XkbDescPtr xkb, unsigned which, int numSI)
{
    return __real_XkbAllocCompatMap(xkb, which, numSI);
}

int __wrap_yylex(void)
{
    return __real_yylex();
}

InterpDef *__wrap_InterpCreate(const char *sym_str, ExprDef *match)
{
    return __real_InterpCreate(sym_str, match);
}
