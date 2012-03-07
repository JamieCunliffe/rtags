#include "Indexer.h"
#include "UnitCache.h"
#include "Resource.h"
#include "Path.h"
#include "Database.h"
#include "leveldb/db.h"
#include "leveldb/write_batch.h"
#include <QHash>
#include <QThreadPool>
#include <QRunnable>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QVector>
#include <QDir>
#include <QDebug>

#define SYNCINTERVAL 10

class IndexerJob;

typedef QHash<QByteArray, QSet<QByteArray> > HashSet;

class IndexerImpl
{
public:
    int jobCounter;
    void syncData(QMutex* mutex, HashSet& data, Database::Type type);

    QMutex implMutex;
    QSet<QByteArray> indexing;

    QByteArray path;
    int lastJobId;
    QHash<int, IndexerJob*> jobs;

    QMutex incMutex;
    HashSet incs;

    QMutex defMutex;
    HashSet defs;

    QMutex refMutex;
    HashSet refs;

    QMutex symMutex;
    HashSet syms;
};

void IndexerImpl::syncData(QMutex* mutex,
                           HashSet& data,
                           Database::Type type)
{
    leveldb::DB* db = 0;
    leveldb::Options options;
    options.create_if_missing = true;
    QByteArray name = Database::databaseName(type);
    if (name.isEmpty())
        return;

    leveldb::Status status = leveldb::DB::Open(options, name.constData(), &db);
    if (!status.ok())
        return;
    Q_ASSERT(db);

    QMutexLocker locker(mutex);

    leveldb::WriteBatch batch;
    const leveldb::ReadOptions readopts;

    HashSet::iterator it = data.begin();
    const HashSet::const_iterator end = data.end();
    while (it != end) {
        QSet<QByteArray>& set = it.value();

        std::string value;
        db->Get(readopts, it.key().constData(), &value);

        QByteArray bvalue = QByteArray::fromRawData(value.c_str(), value.size());
        QSet<QByteArray> newset = bvalue.split('\n').toSet(), inter;
        newset.remove(QByteArray(""));

        inter = newset & set; // intersection
        if (inter.size() == set.size()) { // if the intersection contains all of our preexisting items then we're good
            ++it;
            continue;
        }
        newset.unite(set);

        value.clear();
        QSet<QByteArray>::const_iterator vit = newset.begin();
        const QSet<QByteArray>::const_iterator vend = newset.end();
        while (vit != vend) {
            value += (*vit).constData();
            value += '\n';
            ++vit;
        }

        batch.Put(it.key().constData(), value);
        ++it;
    }
    data.clear();

    db->Write(leveldb::WriteOptions(), &batch);
    delete db;
}

class IndexerJob : public QObject, public QRunnable
{
    Q_OBJECT
public:
    IndexerJob(IndexerImpl* impl, Indexer::Mode mode, int id, const QByteArray& path,
               const QByteArray& input, const QList<QByteArray>& arguments);

    int id() const { return m_id; }

    void run();

    Indexer::Mode m_mode;
    int m_id;
    QByteArray m_path, m_in;
    QList<QByteArray> m_args;
    IndexerImpl* m_impl;

    HashSet m_defs, m_refs, m_syms;

private:
    void addFilenameSymbol(const QByteArray& filename);

signals:
    void done(int id, const QByteArray& filename);
};

#include "Indexer.moc"

static inline void addInclusion(IndexerJob* job, CXFile inc)
{
    CXString str = clang_getFileName(inc);

    const QByteArray path = Path::resolved(clang_getCString(str));

    QMutexLocker locker(&job->m_impl->incMutex);
    if (!qstrcmp(job->m_in, path)) {
        clang_disposeString(str);
        return;
    }

    job->m_impl->incs[path].insert(job->m_in);
    clang_disposeString(str);
}

static void inclusionVisitor(CXFile included_file,
                             CXSourceLocation* inclusion_stack,
                             unsigned include_len,
                             CXClientData client_data)
{
    IndexerJob* job = static_cast<IndexerJob*>(client_data);
    addInclusion(job, included_file);

    CXFile inf;
    unsigned int inl, inc, ino;

    CXSourceLocation* cur = inclusion_stack;
    const CXSourceLocation* end = cur + include_len;
    while (cur != end) {
        clang_getSpellingLocation(*cur, &inf, &inl, &inc, &ino);
        addInclusion(job, inf);
        ++cur;
    }
}

static inline void addNamePermutations(CXCursor cursor, const char* usr, IndexerJob* job)
{
    QByteArray qusr = QByteArray(usr), qname;
    QByteArray qparam, qnoparam;

    CXString displayName;
    CXCursor cur = cursor, null = clang_getNullCursor();
    CXCursorKind kind;
    for (;;) {
        if (clang_equalCursors(cur, null))
            break;
        kind = clang_getCursorKind(cur);
        if (clang_isTranslationUnit(kind))
            break;

        displayName = clang_getCursorDisplayName(cur);
        const char* name = clang_getCString(displayName);
        if (!name || !strlen(name)) {
            clang_disposeString(displayName);
            break;
        }
        qname = QByteArray(name);
        if (qparam.isEmpty()) {
            qparam.prepend(qname);
            qnoparam.prepend(qname);
            const int sp = qnoparam.indexOf('(');
            if (sp != -1)
                qnoparam = qnoparam.left(sp);
        } else {
            qparam.prepend(qname + "::");
            qnoparam.prepend(qname + "::");
        }
        job->m_syms[qparam].insert(qusr);
        if (qparam != qnoparam)
            job->m_syms[qnoparam].insert(qusr);

        clang_disposeString(displayName);
        cur = clang_getCursorSemanticParent(cur);
    }
}

static CXChildVisitResult indexVisitor(CXCursor cursor,
                                       CXCursor /*parent*/,
                                       CXClientData client_data)
{
    IndexerJob* job = static_cast<IndexerJob*>(client_data);

    CXCursorKind kind = clang_getCursorKind(cursor);
    switch (kind) {
    case CXCursor_CXXAccessSpecifier:
        return CXChildVisit_Recurse;
    default:
        break;
    }

    CXString usr = clang_getCursorUSR(cursor);
    const char* cusr = clang_getCString(usr);
    int usrlen = strlen(cusr);
    if (!usrlen || (usrlen == 2 && !strncmp(cusr, "c:", 2))) {
        clang_disposeString(usr);
        CXCursor ref = clang_getCursorReferenced(cursor);
        usr = clang_getCursorUSR(ref);
        cusr = clang_getCString(usr);
        usrlen = strlen(cusr);
        if (!usrlen || (usrlen == 2 && !strncmp(cusr, "c:", 2))) {
            clang_disposeString(usr);
            return CXChildVisit_Recurse;
        }
    }
    //CXString kindstr = clang_getCursorKindSpelling(kind);

    CXSourceLocation loc = clang_getCursorLocation(cursor);
    CXFile file;
    unsigned int line, col;
    clang_getSpellingLocation(loc, &file, &line, &col, 0);
    CXString filename = clang_getFileName(file);
    const char* cfilename = clang_getCString(filename);
    if (!cfilename || !strlen(cfilename)) {
        //clang_disposeString(kindstr);
        clang_disposeString(usr);
        clang_disposeString(filename);
        return CXChildVisit_Recurse;
    }
    QByteArray qloc(Path::resolved(cfilename));
    qloc += ":" + QByteArray::number(line) + ":" + QByteArray::number(col);

    if (clang_isCursorDefinition(cursor)) {
        job->m_defs[cusr].insert(qloc);
        addNamePermutations(cursor, cusr, job);
    }
    job->m_refs[cusr].insert(qloc);

    Q_ASSERT(strcmp(cusr, "") != 0);
    Q_ASSERT(strcmp(cusr, "c:") != 0);
    //clang_disposeString(kindstr);
    clang_disposeString(filename);
    clang_disposeString(usr);

    return CXChildVisit_Recurse;
}

IndexerJob::IndexerJob(IndexerImpl* impl, Indexer::Mode mode, int id, const QByteArray& path,
                       const QByteArray& input, const QList<QByteArray>& arguments)
    : m_mode(mode), m_id(id), m_path(path), m_in(input), m_args(arguments), m_impl(impl)
{
}

inline void IndexerJob::addFilenameSymbol(const QByteArray& filename)
{
    // ### would it be faster/better to use QFileInfo here?
    int idx = -1;
    for (;;) {
        int backslashes = 0;
        idx = filename.lastIndexOf('/', idx);
        while (idx > 0 && filename.at(idx - 1) == '\\') {
            ++backslashes;
            --idx;
        }
        if ((backslashes % 2) || !idx) {
            idx -= 1;
            if (!idx)
                break;
        } else {
            idx += backslashes;
            break;
        }
    }
    if (idx == -1)
        return;
    m_syms[filename.mid(idx + 1)].insert(filename);
}

static inline void uniteSets(HashSet& dst, HashSet& src)
{
    HashSet::const_iterator it = src.begin();
    const HashSet::const_iterator end = src.end();
    while (it != end) {
        dst[it.key()].unite(it.value());
        ++it;
    }
    src.clear();
}

void IndexerJob::run()
{
    int unitMode = UnitCache::Source | UnitCache::AST;
    if (m_mode == Indexer::Force)
        unitMode |= UnitCache::Force;
    CachedUnit unit(m_in, m_args, unitMode);

    if (unit.unit()) {
        qDebug() << "parsing" << m_in << unit.unit()->filename;
        CXTranslationUnit tu = unit.unit()->unit;
        unsigned int diagCount = clang_getNumDiagnostics(tu);
        for (unsigned int i = 0; i < diagCount; ++i) {
            const CXDiagnostic diag = clang_getDiagnostic(tu, i);
            const CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(diag);
            if (severity >= CXDiagnostic_Warning) {
                CXString msg = clang_formatDiagnostic(diag, CXDiagnostic_DisplaySourceLocation
                                                      | CXDiagnostic_DisplayColumn
                                                      | CXDiagnostic_DisplayOption
                                                      | CXDiagnostic_DisplayCategoryName);
                qWarning("clang: %s", clang_getCString(msg));
                clang_disposeString(msg);
            }
            clang_disposeDiagnostic(diag);
        }

        if (unit.unit()->origin == UnitCache::Source) {
            qDebug() << "reread" << unit.unit()->filename << "from source, revisiting";
            clang_getInclusions(tu, inclusionVisitor, this);
            clang_visitChildren(clang_getTranslationUnitCursor(tu), indexVisitor, this);
            addFilenameSymbol(unit.unit()->filename);

            QMutexLocker deflocker(&m_impl->defMutex);
            uniteSets(m_impl->defs, m_defs);
            deflocker.unlock();
            QMutexLocker reflocker(&m_impl->refMutex);
            uniteSets(m_impl->refs, m_refs);
            reflocker.unlock();
            QMutexLocker symlocker(&m_impl->symMutex);
            uniteSets(m_impl->syms, m_syms);
            symlocker.unlock();
        }
    } else {
        qDebug() << "got 0 unit for" << m_in;
    }

    emit done(m_id, m_in);
}

Indexer* Indexer::s_inst = 0;

Indexer::Indexer(const QByteArray& path, QObject* parent)
    : QObject(parent), m_impl(new IndexerImpl)
{
    Q_ASSERT(path.startsWith('/'));
    if (!path.startsWith('/'))
        return;
    QDir dir;
    dir.mkpath(path);

    m_impl->jobCounter = 0;
    m_impl->lastJobId = 0;
    m_impl->path = path;

    s_inst = this;
}

Indexer::~Indexer()
{
    s_inst = 0;

    delete m_impl;
}

Indexer* Indexer::instance()
{
    return s_inst;
}

int Indexer::index(const QByteArray& input, const QList<QByteArray>& arguments, Mode mode)
{
    QMutexLocker locker(&m_impl->implMutex);

    if (m_impl->indexing.contains(input))
        return -1;

    int id;
    do {
        id = m_impl->lastJobId++;
    } while (m_impl->jobs.contains(id));

    m_impl->indexing.insert(input);

    IndexerJob* job = new IndexerJob(m_impl, mode, id, m_impl->path, input, arguments);
    m_impl->jobs[id] = job;
    connect(job, SIGNAL(done(int, const QByteArray&)),
            this, SLOT(jobDone(int, const QByteArray&)), Qt::QueuedConnection);
    QThreadPool::globalInstance()->start(job);

    return id;
}

int Indexer::reindex(const QByteArray& filename, Mode mode)
{
    Resource resource(filename);
    qDebug() << "trying to reindex" << filename << resource.hashedFilename(Resource::Information);
    if (!resource.exists(Resource::Information)) {
        qDebug() << "but resource does not exist";
        return -1;
    }

    QList<QByteArray> data = resource.read<QList<QByteArray> >(Resource::Information);
    if (data.isEmpty() || data.at(0).isEmpty()) {
        qDebug() << "no resource data";
        return -1;
    }

    QByteArray input = data.takeFirst();

    qDebug() << "reindexing" << input << data;

    return index(input, data, mode);
}

void Indexer::jobDone(int id, const QByteArray& filename)
{
    QMutexLocker locker(&m_impl->implMutex);

    m_impl->jobs.remove(id);
    m_impl->indexing.remove(filename);

    ++m_impl->jobCounter;

    if (m_impl->jobs.isEmpty() || m_impl->jobCounter == SYNCINTERVAL) {
        qDebug() << "syncing";
        m_impl->syncData(&m_impl->incMutex, m_impl->incs, Database::Include);
        m_impl->syncData(&m_impl->defMutex, m_impl->defs, Database::Definition);
        m_impl->syncData(&m_impl->refMutex, m_impl->refs, Database::Reference);
        m_impl->syncData(&m_impl->symMutex, m_impl->syms, Database::Symbol);
        qDebug() << "synced";
        m_impl->jobCounter = 0;
    }

    emit indexingDone(id);
}
