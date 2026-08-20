/*
 * fts5_cjk.cpp — "cjk_unicode61" FTS5 tokenizer (loadable SQLite extension).
 *
 * Wraps the stock unicode61 tokenizer: every token unicode61 emits is
 * re-examined and its maximal CJK runs are re-emitted as overlapping
 * character BIGRAMS (Lucene CJKAnalyzer semantics); non-CJK segments pass
 * through unchanged; a lone CJK char becomes a unigram. This gives
 * index-speed matching for 2-char CJK terms that the stock unicode61
 * (whole-run token) and trigram (>=3 chars) tokenizers cannot serve.
 *
 * Build: xmake target "fts5_cjk" (see src/xmake.lua). Vendored public-domain
 * SQLite headers in vendor/ (sqlite3.h + sqlite3ext.h) make the build work
 * without a system libsqlite3-dev.
 *
 * Load: conn.load_extension(path)  # entrypoint sqlite3_ftscjk_init
 * Use:  CREATE VIRTUAL TABLE t USING fts5(c, tokenize='cjk_unicode61');
 * Extra args pass through to unicode61:
 *   tokenize='cjk_unicode61 remove_diacritics 2'
 *
 * Ported from Hermes-CN-Core native/fts5_cjk/fts5_cjk.c (PR #65544,
 * contributed by Soju06), adapted to C++ and the kimix native tree.
 */
#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#include "fts5_cjk_core.h"

#include <cstring>

namespace {

using kimix::native::fts5_cjk::TokenSink;
using kimix::native::fts5_cjk::emit_cjk_bigrams;

struct CjkTokenizer {
    fts5_tokenizer inner;   // unicode61 methods
    Fts5Tokenizer* pInner;  // unicode61 instance
};

struct CjkCallbackCtx {
    void* pOuterCtx;
    int (*xOuterToken)(void*, int, const char*, int, int, int);
};

// Bridge: unicode61's callback -> our bigram re-emitter -> the outer sink.
int cjkInnerCallback(void* pCtx, int tflags,
                     const char* pToken, int nToken,
                     int iStart, int iEnd) {
    CjkCallbackCtx* p = static_cast<CjkCallbackCtx*>(pCtx);
    return emit_cjk_bigrams(p->pOuterCtx, p->xOuterToken,
                            tflags, pToken, nToken, iStart, iEnd);
}

int cjkCreate(void* pApiCtx, const char** azArg, int nArg,
              Fts5Tokenizer** ppOut) {
    fts5_api* pApi = static_cast<fts5_api*>(pApiCtx);
    CjkTokenizer* p = static_cast<CjkTokenizer*>(sqlite3_malloc(sizeof(CjkTokenizer)));
    if (!p) {
        return SQLITE_NOMEM;
    }
    std::memset(p, 0, sizeof(*p));
    void* pInnerCtx = nullptr;
    int rc = pApi->xFindTokenizer(pApi, "unicode61", &pInnerCtx, &p->inner);
    if (rc == SQLITE_OK) {
        rc = p->inner.xCreate(pInnerCtx, azArg, nArg, &p->pInner);
    }
    if (rc != SQLITE_OK) {
        sqlite3_free(p);
        return rc;
    }
    *ppOut = reinterpret_cast<Fts5Tokenizer*>(p);
    return SQLITE_OK;
}

void cjkDelete(Fts5Tokenizer* pTok) {
    CjkTokenizer* p = reinterpret_cast<CjkTokenizer*>(pTok);
    if (p) {
        if (p->pInner) {
            p->inner.xDelete(p->pInner);
        }
        sqlite3_free(p);
    }
}

int cjkTokenize(Fts5Tokenizer* pTok, void* pCtx, int flags,
                const char* pText, int nText,
                int (*xToken)(void*, int, const char*, int, int, int)) {
    CjkTokenizer* p = reinterpret_cast<CjkTokenizer*>(pTok);
    CjkCallbackCtx cb;
    cb.pOuterCtx = pCtx;
    cb.xOuterToken = xToken;
    return p->inner.xTokenize(p->pInner, &cb, flags, pText, nText,
                              cjkInnerCallback);
}

fts5_api* cjkFts5Api(sqlite3* db) {
    fts5_api* pRet = nullptr;
    sqlite3_stmt* pStmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT fts5(?1)", -1, &pStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_pointer(pStmt, 1, static_cast<void*>(&pRet), "fts5_api_ptr", nullptr);
        sqlite3_step(pStmt);
    }
    sqlite3_finalize(pStmt);
    return pRet;
}

} // namespace

extern "C" {

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_ftscjk_init(sqlite3* db, char** pzErrMsg,
                        const sqlite3_api_routines* pApi) {
    static fts5_tokenizer tok = { cjkCreate, cjkDelete, cjkTokenize };
    SQLITE_EXTENSION_INIT2(pApi);
    (void)pzErrMsg;
    fts5_api* pFts = cjkFts5Api(db);
    if (!pFts) {
        if (pzErrMsg) {
            *pzErrMsg = sqlite3_mprintf("fts5_cjk: FTS5 unavailable");
        }
        return SQLITE_ERROR;
    }
    return pFts->xCreateTokenizer(pFts, "cjk_unicode61",
                                  static_cast<void*>(pFts), &tok, nullptr);
}

// Alias for callers that spell out the underscored basename.
#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_fts5_cjk_init(sqlite3* db, char** pzErrMsg,
                          const sqlite3_api_routines* pApi) {
    return sqlite3_ftscjk_init(db, pzErrMsg, pApi);
}

} // extern "C"
