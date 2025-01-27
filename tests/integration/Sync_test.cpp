/**
 * @file tests/synctests.cpp
 * @brief Mega SDK test file
 *
 * (c) 2018 by Mega Limited, Wellsford, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

// Many of these tests are still being worked on.
// The file uses some C++17 mainly for the very convenient std::filesystem library, though the main SDK must still build with C++11 (and prior)


#include "test.h"
#include "stdfs.h"
#include <mega.h>
#include "gtest/gtest.h"
#include <stdio.h>
#include <map>
#include <future>
//#include <mega/tsthooks.h>
#include <fstream>
#include <atomic>
#include <random>

#include <megaapi_impl.h>

#define DEFAULTWAIT std::chrono::seconds(20)

using namespace ::mega;
using namespace ::std;


typedef std::shared_ptr<promise<bool>> PromiseBoolSP;

PromiseBoolSP newPromiseBoolSP()
{
    return PromiseBoolSP(new promise<bool>());
}



#ifdef ENABLE_SYNC

namespace {

bool suppressfiles = false;

typedef ::mega::byte byte;

// Creates a temporary directory in the current path
fs::path makeTmpDir(const int maxTries = 1000)
{
    const auto cwd = fs::current_path();
    std::random_device dev;
    std::mt19937 prng{dev()};
    std::uniform_int_distribution<uint64_t> rand{0};
    fs::path path;
    for (int i = 0;; ++i)
    {
        std::ostringstream os;
        os << std::hex << rand(prng);
        path = cwd / os.str();
        if (fs::create_directory(path))
        {
            break;
        }
        if (i == maxTries)
        {
            throw std::runtime_error{"Couldn't create tmp dir"};
        }
    }
    return path;
}

// Copies a file while maintaining the write time.
void copyFile(const fs::path& source, const fs::path& target)
{
    assert(fs::is_regular_file(source));
    const auto tmpDir = makeTmpDir();
    const auto tmpFile = tmpDir / "copied_file";
    fs::copy_file(source, tmpFile);
    fs::last_write_time(tmpFile, fs::last_write_time(source));
    fs::rename(tmpFile, target);
    fs::remove(tmpDir);
}

string leafname(const string& p)
{
    auto n = p.find_last_of("/");
    return n == string::npos ? p : p.substr(n+1);
}

string parentpath(const string& p)
{
    auto n = p.find_last_of("/");
    return n == string::npos ? "" : p.substr(0, n-1);
}

void WaitMillisec(unsigned n)
{
#ifdef _WIN32
    if (n > 1000)
    {
        for (int i = 0; i < 10; ++i)
        {
            // better for debugging, with breakpoints, pauses, etc
            Sleep(n/10);
        }
    }
    else
    {
        Sleep(n);
    }
#else
    usleep(n * 1000);
#endif
}

bool createFile(const fs::path &path, const void *data, const size_t data_length)
{
#if (__cplusplus >= 201700L)
    ofstream ostream(path, ios::binary);
#else
    ofstream ostream(path.u8string(), ios::binary);
#endif

    ostream.write(reinterpret_cast<const char *>(data), data_length);

    return ostream.good();
}

bool createDataFile(const fs::path &path, const std::string &data)
{
    return createFile(path, data.data(), data.size());
}

struct Model
{
    // records what we think the tree should look like after sync so we can confirm it

    struct ModelNode
    {
        enum nodetype { file, folder };
        nodetype type = folder;
        string name;
        string content;
        vector<unique_ptr<ModelNode>> kids;
        ModelNode* parent = nullptr;
        bool changed = false;

        ModelNode() = default;

        ModelNode(const ModelNode& other)
          : type(other.type)
          , name(other.name)
          , content(other.content)
          , kids()
          , parent()
          , changed(other.changed)
        {
            for (auto& child : other.kids)
            {
                addkid(child->clone());
            }
        }

        void generate(const fs::path& path)
        {
            const fs::path ourPath = path / name;

            if (type == file)
            {
                if (changed)
                {
                    ASSERT_TRUE(createDataFile(ourPath, content));
                    changed = false;
                }
            }
            else
            {
                fs::create_directory(ourPath);

                for (auto& child : kids)
                {
                    child->generate(ourPath);
                }
            }
        }

        string path()
        {
            string s;
            for (auto p = this; p; p = p->parent)
                s = "/" + p->name + s;
            return s;
        }

        ModelNode* addkid()
        {
            return addkid(::mega::make_unique<ModelNode>());
        }

        ModelNode* addkid(unique_ptr<ModelNode>&& p)
        {
            p->parent = this;
            kids.emplace_back(move(p));

            return kids.back().get();
        }

        bool typematchesnodetype(nodetype_t nodetype)
        {
            switch (type)
            {
            case file: return nodetype == FILENODE;
            case folder: return nodetype == FOLDERNODE;
            }
            return false;
        }

        void print(string prefix="")
        {
            out() << prefix << name << endl;
            prefix.append(name).append("/");
            for (const auto &in: kids)
            {
                in->print(prefix);
            }
        }

        std::unique_ptr<ModelNode> clone()
        {
            return ::mega::make_unique<ModelNode>(*this);
        }
    };

    Model()
      : root(makeModelSubfolder("root"))
    {
    }

    Model(const Model& other)
      : root(other.root->clone())
    {
    }

    Model& operator=(const Model& rhs)
    {
        Model temp(rhs);

        swap(temp);

        return *this;
    }

    ModelNode* addfile(const string& path, const string& content)
    {
        auto* node = addnode(path, ModelNode::file);

        node->content = content;
        node->changed = true;

        return node;
    }

    ModelNode* addfile(const string& path)
    {
        return addfile(path, path);
    }

    ModelNode* addfolder(const string& path)
    {
        return addnode(path, ModelNode::folder);
    }

    ModelNode* addnode(const string& path, ModelNode::nodetype type)
    {
        ModelNode* child;
        ModelNode* node = root.get();
        string name;
        size_t current = 0;
        size_t end = path.size();

        while (current < end)
        {
            size_t delimiter = path.find('/', current);

            if (delimiter == path.npos)
            {
                break;
            }

            name = path.substr(current, delimiter - current);

            if (!(child = childnodebyname(node, name)))
            {
                child = node->addkid();

                child->name = name;
                child->type = ModelNode::folder;
            }

            assert(child->type == ModelNode::folder);

            current = delimiter + 1;
            node = child;
        }

        assert(current < end);

        name = path.substr(current);

        if (!(child = childnodebyname(node, name)))
        {
            child = node->addkid();

            child->name = name;
            child->type = type;
        }

        assert(child->type == type);

        return child;
    }

    ModelNode* copynode(const string& src, const string& dst)
    {
        const ModelNode* source = findnode(src);
        ModelNode* destination = addnode(dst, source->type);

        destination->content = source->content;
        destination->kids.clear();

        for (auto& child : source->kids)
        {
            destination->addkid(child->clone());
        }

        return destination;
    }

    unique_ptr<ModelNode> makeModelSubfolder(const string& utf8Name)
    {
        unique_ptr<ModelNode> n(new ModelNode);
        n->name = utf8Name;
        return n;
    }

    unique_ptr<ModelNode> makeModelSubfile(const string& utf8Name, string content = {})
    {
        unique_ptr<ModelNode> n(new ModelNode);
        n->name = utf8Name;
        n->type = ModelNode::file;
        n->content = content.empty() ? utf8Name : std::move(content);
        return n;
    }

    unique_ptr<ModelNode> buildModelSubdirs(const string& prefix, int n, int recurselevel, int filesperdir)
    {
        if (suppressfiles) filesperdir = 0;

        unique_ptr<ModelNode> nn = makeModelSubfolder(prefix);

        for (int i = 0; i < filesperdir; ++i)
        {
            nn->addkid(makeModelSubfile("file" + to_string(i) + "_" + prefix));
        }

        if (recurselevel > 0)
        {
            for (int i = 0; i < n; ++i)
            {
                unique_ptr<ModelNode> sn = buildModelSubdirs(prefix + "_" + to_string(i), n, recurselevel - 1, filesperdir);
                sn->parent = nn.get();
                nn->addkid(move(sn));
            }
        }
        return nn;
    }

    ModelNode* childnodebyname(ModelNode* n, const std::string& s)
    {
        for (auto& m : n->kids)
        {
            if (m->name == s)
            {
                return m.get();
            }
        }
        return nullptr;
    }

    ModelNode* findnode(string path, ModelNode* startnode = nullptr)
    {
        ModelNode* n = startnode ? startnode : root.get();
        while (n && !path.empty())
        {
            auto pos = path.find("/");
            n = childnodebyname(n, path.substr(0, pos));
            path.erase(0, pos == string::npos ? path.size() : pos + 1);
        }
        return n;
    }

    unique_ptr<ModelNode> removenode(const string& path)
    {
        ModelNode* n = findnode(path);
        if (n && n->parent)
        {
            unique_ptr<ModelNode> extracted;
            ModelNode* parent = n->parent;
            auto newend = std::remove_if(parent->kids.begin(), parent->kids.end(), [&extracted, n](unique_ptr<ModelNode>& v) { if (v.get() == n) return extracted = move(v), true; else return false; });
            parent->kids.erase(newend, parent->kids.end());
            return extracted;
        }
        return nullptr;
    }

    bool movenode(const string& sourcepath, const string& destpath)
    {
        ModelNode* source = findnode(sourcepath);
        ModelNode* dest = findnode(destpath);
        if (source && source && source->parent && dest)
        {
            auto replaced_node = removenode(destpath + "/" + source->name);

            unique_ptr<ModelNode> n;
            ModelNode* parent = source->parent;
            auto newend = std::remove_if(parent->kids.begin(), parent->kids.end(), [&n, source](unique_ptr<ModelNode>& v) { if (v.get() == source) return n = move(v), true; else return false; });
            parent->kids.erase(newend, parent->kids.end());
            if (n)
            {
                dest->addkid(move(n));
                return true;
            }
        }
        return false;
    }

    bool movetosynctrash(const string& path, const string& syncrootpath)
    {
        ModelNode* syncroot;
        if (!(syncroot = findnode(syncrootpath)))
        {
            return false;
        }

        ModelNode* trash;
        if (!(trash = childnodebyname(syncroot, DEBRISFOLDER)))
        {
            auto uniqueptr = makeModelSubfolder(DEBRISFOLDER);
            trash = uniqueptr.get();
            syncroot->addkid(move(uniqueptr));
        }

        char today[50];
        auto rawtime = time(NULL);
        strftime(today, sizeof today, "%F", localtime(&rawtime));

        ModelNode* dayfolder;
        if (!(dayfolder = findnode(today, trash)))
        {
            auto uniqueptr = makeModelSubfolder(today);
            dayfolder = uniqueptr.get();
            trash->addkid(move(uniqueptr));
        }

        if (auto uniqueptr = removenode(path))
        {
            dayfolder->addkid(move(uniqueptr));
            return true;
        }
        return false;
    }

    void ensureLocalDebrisTmpLock(const string& syncrootpath)
    {
        // if we've downloaded a file then it's put in debris/tmp initially, and there is a lock file
        if (ModelNode* syncroot = findnode(syncrootpath))
        {
            ModelNode* trash;
            if (!(trash = childnodebyname(syncroot, DEBRISFOLDER)))
            {
                auto uniqueptr = makeModelSubfolder(DEBRISFOLDER);
                trash = uniqueptr.get();
                syncroot->addkid(move(uniqueptr));
            }

            ModelNode* tmpfolder;
            if (!(tmpfolder = findnode("tmp", trash)))
            {
                auto uniqueptr = makeModelSubfolder("tmp");
                tmpfolder = uniqueptr.get();
                trash->addkid(move(uniqueptr));
            }

            ModelNode* lockfile;
            if (!(lockfile = findnode("lock", tmpfolder)))
            {
                tmpfolder->addkid(makeModelSubfile("lock"));
            }
        }
    }

    bool removesynctrash(const string& syncrootpath, const string& subpath = "")
    {
        if (subpath.empty())
        {
            return removenode(syncrootpath + "/" + DEBRISFOLDER).get();
        }
        else
        {
            char today[50];
            auto rawtime = time(NULL);
            strftime(today, sizeof today, "%F", localtime(&rawtime));

            return removenode(syncrootpath + "/" + DEBRISFOLDER + "/" + today + "/" + subpath).get();
        }
    }

    void emulate_rename(std::string nodepath, std::string newname)
    {
        auto node = findnode(nodepath);
        ASSERT_TRUE(!!node);
        if (node) node->name = newname;
    }

    void emulate_move(std::string nodepath, std::string newparentpath)
    {
        auto removed = removenode(newparentpath + "/" + leafname(nodepath));

        ASSERT_TRUE(movenode(nodepath, newparentpath));
    }

    void emulate_copy(std::string nodepath, std::string newparentpath)
    {
        auto node = findnode(nodepath);
        auto newparent = findnode(newparentpath);
        ASSERT_TRUE(!!node);
        ASSERT_TRUE(!!newparent);
        newparent->addkid(node->clone());
    }

    void emulate_rename_copy(std::string nodepath, std::string newparentpath, std::string newname)
    {
        auto node = findnode(nodepath);
        auto newparent = findnode(newparentpath);
        ASSERT_TRUE(!!node);
        ASSERT_TRUE(!!newparent);
        auto newnode = node->clone();
        newnode->name = newname;
        newparent->addkid(std::move(newnode));
    }

    void emulate_delete(std::string nodepath)
    {
        auto removed = removenode(nodepath);
       // ASSERT_TRUE(!!removed);
    }

    void generate(const fs::path& path)
    {
        fs::create_directories(path);

        for (auto& child : root->kids)
        {
            child->generate(path);
        }
    }

    void swap(Model& other)
    {
        using std::swap;

        swap(root, other.root);
    }

    unique_ptr<ModelNode> root;
};


bool waitonresults(future<bool>* r1 = nullptr, future<bool>* r2 = nullptr, future<bool>* r3 = nullptr, future<bool>* r4 = nullptr)
{
    if (r1) r1->wait();
    if (r2) r2->wait();
    if (r3) r3->wait();
    if (r4) r4->wait();
    return (!r1 || r1->get()) && (!r2 || r2->get()) && (!r3 || r3->get()) && (!r4 || r4->get());
}

atomic<int> next_request_tag{ 1 << 30 };

struct StandardClient : public MegaApp
{
    WAIT_CLASS waiter;
#ifdef GFX_CLASS
    GFX_CLASS gfx;
#endif

    string client_dbaccess_path;
    std::unique_ptr<HttpIO> httpio;
    std::unique_ptr<FileSystemAccess> fsaccess;
    std::recursive_mutex clientMutex;
    MegaClient client;
    std::atomic<bool> clientthreadexit{false};
    bool fatalerror = false;
    string clientname;
    std::function<void()> nextfunctionMC;
    std::function<void()> nextfunctionSC;
    std::condition_variable functionDone;
    std::mutex functionDoneMutex;
    std::string salt;
    std::set<fs::path> localFSFilesThatMayDiffer;

    fs::path fsBasePath;

    handle basefolderhandle = UNDEF;

    enum resultprocenum { PRELOGIN, LOGIN, FETCHNODES, PUTNODES, UNLINK, MOVENODE, CATCHUP };

    struct ResultProc
    {
        StandardClient& client;
        ResultProc(StandardClient& c) : client(c) {}

        struct id_callback
        {
            int request_tag = 0;
            handle h = UNDEF;
            std::function<bool(error)> f;
            id_callback(std::function<bool(error)> cf, int tag, handle ch) : request_tag(tag), h(ch), f(cf) {}
        };

        recursive_mutex mtx;  // recursive because sometimes we need to set up new operations during a completion callback
        map<resultprocenum, deque<id_callback>> m;

        void prepresult(resultprocenum rpe, int tag, std::function<void()>&& requestfunc, std::function<bool(error)>&& f, handle h = UNDEF)
        {
            {
                lock_guard<recursive_mutex> g(mtx);
                auto& entry = m[rpe];
                entry.emplace_back(move(f), tag, h);
            }

            std::lock_guard<std::recursive_mutex> lg(client.clientMutex);

            assert(tag > 0);
            int oldtag = client.client.reqtag;
            client.client.reqtag = tag;
            requestfunc();
            client.client.reqtag = oldtag;

            client.client.waiter->notify();
        }

        void processresult(resultprocenum rpe, error e, handle h = UNDEF)
        {
            int tag = client.client.restag;
            if (tag == 0 && rpe != CATCHUP)
            {
                //out() << "received notification of SDK initiated operation " << rpe << " tag " << tag << endl; // too many of those to output
                return;
            }

            if (tag < (2 << 30))
            {
                out() << "ignoring callback from SDK internal sync operation " << rpe << " tag " << tag << endl;
                return;
            }

            lock_guard<recursive_mutex> g(mtx);
            auto& entry = m[rpe];

            if (rpe == CATCHUP)
            {
                while (!entry.empty())
                {
                    entry.front().f(e);
                    entry.pop_front();
                }
                return;
            }

            if (entry.empty())
            {
                out() << "received notification of operation type " << rpe << " completion but we don't have a record of it.  tag: " << tag << endl;
                return;
            }

            if (tag != entry.front().request_tag)
            {
                out() << "tag mismatch for operation completion of " << rpe << " tag " << tag << ", we expected " << entry.front().request_tag << endl;
                return;
            }

            if (entry.front().f(e))
            {
                entry.pop_front();
            }
        }
    } resultproc;

    // thread as last member so everything else is initialised before we start it
    std::thread clientthread;

    string ensureDir(const fs::path& p)
    {
        fs::create_directories(p);

        string result = p.u8string();

        if (result.back() != fs::path::preferred_separator)
        {
            result += fs::path::preferred_separator;
        }

        return result;
    }

    StandardClient(const fs::path& basepath, const string& name)
        : client_dbaccess_path(ensureDir(basepath / name))
        , httpio(new HTTPIO_CLASS)
        , fsaccess(new FSACCESS_CLASS)
        , client(this,
                 &waiter,
                 httpio.get(),
                 fsaccess.get(),
#ifdef DBACCESS_CLASS
                 new DBACCESS_CLASS(LocalPath::fromPath(client_dbaccess_path, *fsaccess)),
#else
                 NULL,
#endif
#ifdef GFX_CLASS
                 &gfx,
#else
                 NULL,
#endif
                 "N9tSBJDC",
                 USER_AGENT.c_str(),
                 THREADS_PER_MEGACLIENT)
        , clientname(name)
        , fsBasePath(basepath / fs::u8path(name))
        , resultproc(*this)
        , clientthread([this]() { threadloop(); })
    {
        client.clientname = clientname + " ";
#ifdef GFX_CLASS
        gfx.startProcessingThread();
#endif
    }

    ~StandardClient()
    {
        // shut down any syncs on the same thread, or they stall the client destruction (CancelIo instead of CancelIoEx on the WinDirNotify)
        auto result =
          thread_do<bool>([](MegaClient& mc, PromiseBoolSP result)
                          {
                              mc.logout(false);
                              result->set_value(true);
                          });

        // Make sure logout completes before we escape.
        result.get();

        clientthreadexit = true;
        waiter.notify();
        clientthread.join();
    }

    void localLogout()
    {
        auto result =
          thread_do<bool>([](MegaClient& mc, PromiseBoolSP result)
                          {
                              mc.locallogout(false, true);
                              result->set_value(true);
                          });

        // Make sure logout completes before we escape.
        result.get();
    }

    static mutex om;
    bool logcb = false;
    chrono::steady_clock::time_point lastcb = std::chrono::steady_clock::now();

    string lp(LocalNode* ln) { return ln->getLocalPath().toName(*client.fsaccess, FS_UNKNOWN); }

    void onCallback() { lastcb = chrono::steady_clock::now(); };

    void syncupdate_stateconfig(handle backupId) override { onCallback(); if (logcb) { lock_guard<mutex> g(om);  out() << clientname << " syncupdate_stateconfig() " << backupId << endl; } }
    void syncupdate_scanning(bool b) override { if (logcb) { onCallback(); lock_guard<mutex> g(om); out() << clientname << " syncupdate_scanning()" << b << endl; } }
    //void syncupdate_local_folder_addition(Sync* s, LocalNode* ln, const char* cp) override { onCallback(); if (logcb) { lock_guard<mutex> g(om); out() << clientname << " syncupdate_local_folder_addition() " << lp(ln) << " " << cp << endl; }}
    //void syncupdate_local_folder_deletion(Sync*, LocalNode* ln) override { if (logcb) { onCallback(); lock_guard<mutex> g(om);  out() << clientname << " syncupdate_local_folder_deletion() " << lp(ln) << endl; }}
    void syncupdate_local_folder_addition(Sync*, LocalNode* ln, const char* cp) override { onCallback(); }
    void syncupdate_local_folder_deletion(Sync*, LocalNode* ln) override { onCallback(); }
    void syncupdate_local_file_addition(Sync*, LocalNode* ln, const char* cp) override { onCallback(); if (logcb) { lock_guard<mutex> g(om); out() << clientname << " syncupdate_local_file_addition() " << lp(ln) << " " << cp << endl; }}
    void syncupdate_local_file_deletion(Sync*, LocalNode* ln) override { if (logcb) { onCallback(); lock_guard<mutex> g(om); out() << clientname << " syncupdate_local_file_deletion() " << lp(ln) << endl; }}
    void syncupdate_local_file_change(Sync*, LocalNode* ln, const char* cp) override { onCallback(); if (logcb) { lock_guard<mutex> g(om); out() << clientname << " syncupdate_local_file_change() " << lp(ln) << " " << cp << endl; }}
    void syncupdate_local_move(Sync*, LocalNode* ln, const char* cp) override { onCallback(); if (logcb) { lock_guard<mutex> g(om); out() << clientname << " syncupdate_local_move() " << lp(ln) << " " << cp << endl; }}
    void syncupdate_local_lockretry(bool b) override { if (logcb) { onCallback(); lock_guard<mutex> g(om); out() << clientname << " syncupdate_local_lockretry() " << b << endl; }}
    //void syncupdate_get(Sync*, Node* n, const char* cp) override { onCallback(); if (logcb) { lock_guard<mutex> g(om); out() << clientname << " syncupdate_get()" << n->displaypath() << " " << cp << endl; }}
    void syncupdate_put(Sync*, LocalNode* ln, const char* cp) override { onCallback(); if (logcb) { lock_guard<mutex> g(om); out() << clientname << " syncupdate_put()" << lp(ln) << " " << cp << endl; }}
    void syncupdate_remote_file_addition(Sync*, Node* n) override { onCallback(); if (logcb) { lock_guard<mutex> g(om); out() << clientname << " syncupdate_remote_file_addition() " << n->displaypath() << endl; }}
    void syncupdate_remote_file_deletion(Sync*, Node* n) override { onCallback(); if (logcb) { lock_guard<mutex> g(om); out() << clientname << " syncupdate_remote_file_deletion() " << n->displaypath() << endl; }}
    //void syncupdate_remote_folder_addition(Sync*, Node* n) override { onCallback(); if (logcb) { lock_guard<mutex> g(om); out() << clientname << " syncupdate_remote_folder_addition() " << n->displaypath() << endl; }}
    //void syncupdate_remote_folder_deletion(Sync*, Node* n) override { onCallback(); if (logcb) { lock_guard<mutex> g(om); out() << clientname << " syncupdate_remote_folder_deletion() " << n->displaypath() << endl; }}
    void syncupdate_remote_folder_addition(Sync*, Node* n) override { onCallback(); }
    void syncupdate_remote_folder_deletion(Sync*, Node* n) override { onCallback(); }
    void syncupdate_remote_copy(Sync*, const char* cp) override { onCallback(); if (logcb) { lock_guard<mutex> g(om); out() << clientname << " syncupdate_remote_copy() " << cp << endl; }}
    void syncupdate_remote_move(Sync*, Node* n1, Node* n2) override { onCallback(); if (logcb) { lock_guard<mutex> g(om); out() << clientname << " syncupdate_remote_move() " << n1->displaypath() << " " << n2->displaypath() << endl; }}
    void syncupdate_remote_rename(Sync*, Node* n, const char* cp) override { onCallback(); if (logcb) { lock_guard<mutex> g(om); out() << clientname << " syncupdate_remote_rename() " << n->displaypath() << " " << cp << endl; }}
    //void syncupdate_treestate(LocalNode* ln) override { onCallback(); if (logcb) { lock_guard<mutex> g(om);   out() << clientname << " syncupdate_treestate() " << ln->ts << " " << ln->dts << " " << lp(ln) << endl; }}

    bool sync_syncable(Sync* sync, const char* name, LocalPath& path, Node*) override
    {
        return sync_syncable(sync, name, path);
    }

    bool sync_syncable(Sync*, const char*, LocalPath&) override
    {
        onCallback();

        return true;
    }

    std::atomic<unsigned> transfersAdded{0}, transfersRemoved{0}, transfersPrepared{0}, transfersFailed{0}, transfersUpdated{0}, transfersComplete{0};

    void transfer_added(Transfer*) override { onCallback(); ++transfersAdded; }
    void transfer_removed(Transfer*) override { onCallback(); ++transfersRemoved; }
    void transfer_prepare(Transfer*) override { onCallback(); ++transfersPrepared; }
    void transfer_failed(Transfer*,  const Error&, dstime = 0) override { onCallback(); ++transfersFailed; }
    void transfer_update(Transfer*) override { onCallback(); ++transfersUpdated; }
    void transfer_complete(Transfer*) override { onCallback(); ++transfersComplete; }

    void threadloop()
        try
    {
        while (!clientthreadexit)
        {
            int r;

            {
                std::lock_guard<std::recursive_mutex> lg(clientMutex);
                r = client.preparewait();
            }

            if (!r)
            {
                r |= client.dowait();
            }

            std::lock_guard<std::recursive_mutex> lg(clientMutex);
            r |= client.checkevents();

            {
                std::lock_guard<mutex> g(functionDoneMutex);
                if (nextfunctionMC)
                {
                    nextfunctionMC();
                    nextfunctionMC = nullptr;
                    functionDone.notify_all();
                    r |= Waiter::NEEDEXEC;
                }
                if (nextfunctionSC)
                {
                    nextfunctionSC();
                    nextfunctionSC = nullptr;
                    functionDone.notify_all();
                    r |= Waiter::NEEDEXEC;
                }
            }
            if ((r & Waiter::NEEDEXEC))
            {
                client.exec();
            }
        }
        out() << clientname << " thread exiting naturally" << endl;
    }
    catch (std::exception& e)
    {
        out() << clientname << " thread exception, StandardClient " << clientname << " terminated: " << e.what() << endl;
    }
    catch (...)
    {
        out() << clientname << " thread exception, StandardClient " << clientname << " terminated" << endl;
    }

    static bool debugging;  // turn this on to prevent the main thread timing out when stepping in the MegaClient

    template <class PROMISE_VALUE>
    future<PROMISE_VALUE> thread_do(std::function<void(MegaClient&, std::shared_ptr<promise<PROMISE_VALUE>>)> f)
    {
        unique_lock<mutex> guard(functionDoneMutex);
        std::shared_ptr<promise<PROMISE_VALUE>> promiseSP(new promise<PROMISE_VALUE>());
        nextfunctionMC = [this, promiseSP, f](){ f(this->client, promiseSP); };
        waiter.notify();
        while (!functionDone.wait_until(guard, chrono::steady_clock::now() + chrono::seconds(600), [this]() { return !nextfunctionMC; }))
        {
            if (!debugging)
            {
                promiseSP->set_value(false);
                break;
            }
        }
        return promiseSP->get_future();
    }

    template <class PROMISE_VALUE>
    future<PROMISE_VALUE> thread_do(std::function<void(StandardClient&, std::shared_ptr<promise<PROMISE_VALUE>>)> f)
    {
        unique_lock<mutex> guard(functionDoneMutex);
        std::shared_ptr<promise<PROMISE_VALUE>> promiseSP(new promise<PROMISE_VALUE>());
        nextfunctionMC = [this, promiseSP, f]() { f(*this, promiseSP); };
        waiter.notify();
        while (!functionDone.wait_until(guard, chrono::steady_clock::now() + chrono::seconds(600), [this]() { return !nextfunctionSC; }))
        {
            if (!debugging)
            {
                promiseSP->set_value(false);
                break;
            }
        }
        return promiseSP->get_future();
    }

    void preloginFromEnv(const string& userenv, PromiseBoolSP pb)
    {
        string user = getenv(userenv.c_str());

        ASSERT_FALSE(user.empty());

        resultproc.prepresult(PRELOGIN, ++next_request_tag,
            [&](){ client.prelogin(user.c_str()); },
            [pb](error e) { pb->set_value(!e); return true; });

    }

    void loginFromEnv(const string& userenv, const string& pwdenv, PromiseBoolSP pb)
    {
        string user = getenv(userenv.c_str());
        string pwd = getenv(pwdenv.c_str());

        ASSERT_FALSE(user.empty());
        ASSERT_FALSE(pwd.empty());

        byte pwkey[SymmCipher::KEYLENGTH];

        resultproc.prepresult(LOGIN, ++next_request_tag,
            [&](){
                if (client.accountversion == 1)
                {
                    if (error e = client.pw_key(pwd.c_str(), pwkey))
                    {
                        ASSERT_TRUE(false) << "login error: " << e;
                    }
                    else
                    {
                        client.login(user.c_str(), pwkey);
                    }
                }
                else if (client.accountversion == 2 && !salt.empty())
                {
                    client.login2(user.c_str(), pwd.c_str(), &salt);
                }
                else
                {
                    ASSERT_TRUE(false) << "Login unexpected error";
                }
            },
            [pb](error e) { pb->set_value(!e); return true; });

    }

    void loginFromSession(const string& session, PromiseBoolSP pb)
    {
        resultproc.prepresult(LOGIN, ++next_request_tag,
            [&](){ client.login(session); },
            [pb](error e) { pb->set_value(!e);  return true; });
    }

    void cloudCopyTreeAs(Node* n1, Node* n2, std::string newname, PromiseBoolSP pb)
    {
        resultproc.prepresult(PUTNODES, ++next_request_tag,
            [&](){
                TreeProcCopy tc;
                client.proctree(n1, &tc, false, true);
                tc.allocnodes();
                client.proctree(n1, &tc, false, true);
                tc.nn[0].parenthandle = UNDEF;

                SymmCipher key;
                AttrMap attrs;
                string attrstring;
                key.setkey((const ::mega::byte*)tc.nn[0].nodekey.data(), n1->type);
                attrs = n1->attrs;
                client.fsaccess->normalize(&newname);
                attrs.map['n'] = newname;
                attrs.getjson(&attrstring);
                client.makeattr(&key, tc.nn[0].attrstring, attrstring.c_str());
                client.putnodes(n2->nodehandle, move(tc.nn));
            },
            [pb](error e) {
                pb->set_value(!e);
                return true;
            });
    }

    void uploadFolderTree_recurse(handle parent, handle& h, const fs::path& p, vector<NewNode>& newnodes)
    {
        NewNode n;
        client.putnodes_prepareOneFolder(&n, p.filename().u8string());
        handle thishandle = n.nodehandle = h++;
        n.parenthandle = parent;
        newnodes.emplace_back(std::move(n));

        for (fs::directory_iterator i(p); i != fs::directory_iterator(); ++i)
        {
            if (fs::is_directory(*i))
            {
                uploadFolderTree_recurse(thishandle, h, *i, newnodes);
            }
        }
    }

    void uploadFolderTree(fs::path p, Node* n2, PromiseBoolSP pb)
    {
        resultproc.prepresult(PUTNODES, ++next_request_tag,
            [&](){
                vector<NewNode> newnodes;
                handle h = 1;
                uploadFolderTree_recurse(UNDEF, h, p, newnodes);
                client.putnodes(n2->nodehandle, move(newnodes));
            },
            [pb](error e) { pb->set_value(!e);  return true; });
    }

    // Necessary to make sure we release the file once we're done with it.
    struct FilePut : public File {
        void completed(Transfer* t, LocalNode* n) override
        {
            File::completed(t, n);
            delete this;
        }

        void terminated() override
        {
            delete this;
        }
    }; // FilePut

    void uploadFilesInTree_recurse(Node* target, const fs::path& p, std::atomic<int>& inprogress, DBTableTransactionCommitter& committer)
    {
        if (fs::is_regular_file(p))
        {
            ++inprogress;
            File* f = new FilePut();
            // full local path
            f->localname = LocalPath::fromPath(p.u8string(), *client.fsaccess);
            f->h = target->nodeHandle();
            f->name = p.filename().u8string();
            client.startxfer(PUT, f, committer);
        }
        else if (fs::is_directory(p))
        {
            if (auto newtarget = client.childnodebyname(target, p.filename().u8string().c_str()))
            {
                for (fs::directory_iterator i(p); i != fs::directory_iterator(); ++i)
                {
                    uploadFilesInTree_recurse(newtarget, *i, inprogress, committer);
                }
            }
        }
    }


    void uploadFilesInTree(fs::path p, Node* n2, std::atomic<int>& inprogress, PromiseBoolSP pb)
    {
        resultproc.prepresult(PUTNODES, ++next_request_tag,
            [&](){
                DBTableTransactionCommitter committer(client.tctable);
                uploadFilesInTree_recurse(n2, p, inprogress, committer);
            },
            [pb, &inprogress](error e)
            {
                if (!--inprogress)
                    pb->set_value(true);
                return !inprogress;
            });
    }



    class TreeProcPrintTree : public TreeProc
    {
    public:
        void proc(MegaClient* client, Node* n) override
        {
            //out() << "fetchnodes tree: " << n->displaypath() << endl;;
        }
    };

    // mark node as removed and notify

    std::function<void (StandardClient& mc, PromiseBoolSP pb)> onFetchNodes;

    void fetchnodes(bool noCache, PromiseBoolSP pb)
    {
        resultproc.prepresult(FETCHNODES, ++next_request_tag,
            [&](){ client.fetchnodes(noCache); },
            [this, pb](error e)
            {
                if (e)
                {
                    pb->set_value(false);
                }
                else
                {
                    TreeProcPrintTree tppt;
                    client.proctree(client.nodebyhandle(client.rootnodes[0]), &tppt);

                    if (onFetchNodes)
                    {
                        onFetchNodes(*this, pb);
                    }
                    else
                    {
                        pb->set_value(true);
                    }
                }
                onFetchNodes = nullptr;
                return true;
            });
    }

    NewNode makeSubfolder(const string& utf8Name)
    {
        NewNode newnode;
        client.putnodes_prepareOneFolder(&newnode, utf8Name);
        return newnode;
    }

    void catchup(PromiseBoolSP pb)
    {
        resultproc.prepresult(CATCHUP, ++next_request_tag,
            [&](){
                client.catchup();
            },
            [pb](error e) {
                if (e)
                {
                    out() << "catchup reports: " << e << endl;
                }
                pb->set_value(!e);
                return true;
            });
    }

    void deleteTestBaseFolder(bool mayneeddeleting, PromiseBoolSP pb)
    {
        if (Node* root = client.nodebyhandle(client.rootnodes[0]))
        {
            if (Node* basenode = client.childnodebyname(root, "mega_test_sync", false))
            {
                if (mayneeddeleting)
                {
                    //out() << "old test base folder found, deleting" << endl;
                    resultproc.prepresult(UNLINK, ++next_request_tag,
                        [&](){ client.unlink(basenode, false, client.reqtag); },
                        [this, pb](error e) {
                            if (e)
                            {
                                out() << "delete of test base folder reply reports: " << e << endl;
                            }
                            deleteTestBaseFolder(false, pb);
                            return true;
                        });
                    return;
                }
                out() << "base folder found, but not expected, failing" << endl;
                pb->set_value(false);
                return;
            }
            else
            {
                //out() << "base folder not found, wasn't present or delete successful" << endl;
                pb->set_value(true);
                return;
            }
        }
        out() << "base folder not found, as root was not found!" << endl;
        pb->set_value(false);
    }

    void ensureTestBaseFolder(bool mayneedmaking, PromiseBoolSP pb)
    {
        if (Node* root = client.nodebyhandle(client.rootnodes[0]))
        {
            if (Node* basenode = client.childnodebyname(root, "mega_test_sync", false))
            {
                if (basenode->type == FOLDERNODE)
                {
                    basefolderhandle = basenode->nodehandle;
                    //out() << clientname << " Base folder: " << Base64Str<MegaClient::NODEHANDLE>(basefolderhandle) << endl;
                    //parentofinterest = Base64Str<MegaClient::NODEHANDLE>(basefolderhandle);
                    pb->set_value(true);
                    return;
                }
            }
            else if (mayneedmaking)
            {
                vector<NewNode> nn(1);
                nn[0] = makeSubfolder("mega_test_sync");

                resultproc.prepresult(PUTNODES, ++next_request_tag,
                    [&](){ client.putnodes(root->nodehandle, move(nn)); },
                    [this, pb](error e) { ensureTestBaseFolder(false, pb); return true; });

                return;
            }
        }
        pb->set_value(false);
    }

    NewNode* buildSubdirs(list<NewNode>& nodes, const string& prefix, int n, int recurselevel)
    {
        nodes.emplace_back(makeSubfolder(prefix));
        auto& nn = nodes.back();
        nn.nodehandle = nodes.size();

        if (recurselevel > 0)
        {
            for (int i = 0; i < n; ++i)
            {
                buildSubdirs(nodes, prefix + "_" + to_string(i), n, recurselevel - 1)->parenthandle = nn.nodehandle;
            }
        }

        return &nn;
    }

    void makeCloudSubdirs(const string& prefix, int depth, int fanout, PromiseBoolSP pb, const string& atpath = "")
    {
        assert(basefolderhandle != UNDEF);

        std::list<NewNode> nodes;
        NewNode* nn = buildSubdirs(nodes, prefix, fanout, depth);
        nn->parenthandle = UNDEF;
        nn->ovhandle = UNDEF;

        Node* atnode = client.nodebyhandle(basefolderhandle);
        if (atnode && !atpath.empty())
        {
            atnode = drillchildnodebyname(atnode, atpath);
        }
        if (!atnode)
        {
            out() << "path not found: " << atpath << endl;
            pb->set_value(false);
        }
        else
        {
            auto nodearray = vector<NewNode>(nodes.size());
            size_t i = 0;
            for (auto n = nodes.begin(); n != nodes.end(); ++n, ++i)
            {
                nodearray[i] = std::move(*n);
            }

            resultproc.prepresult(PUTNODES, ++next_request_tag,
                [&](){ client.putnodes(atnode->nodehandle, move(nodearray)); },
                [pb](error e) {
                    pb->set_value(!e);
                    if (e)
                    {
                        out() << "putnodes result: " << e << endl;
                    }
                    return true;
                });
        }
    }

    struct SyncInfo
    {
        handle h;
        fs::path localpath;
    };

    bool syncSet(handle backupId, SyncInfo& info) const
    {
        if (auto* config = client.syncs.syncConfigByBackupId(backupId))
        {
            info.h = config->getRemoteNode();
            info.localpath = config->getLocalPath().toPath(*client.fsaccess);

            return true;
        }

        return false;
    }

    SyncInfo syncSet(handle backupId)
    {
        SyncInfo result;

        out() << "looking up id " << backupId << '\n';

        client.syncs.forEachUnifiedSync([](UnifiedSync& us){
            out() << " ids are: " << us.mConfig.mBackupId << " with local path '" << us.mConfig.getLocalPath().toPath(*us.mClient.fsaccess) << "'\n";
        });

        bool found = syncSet(backupId, result);
        assert(found);

        return result;
    }

    Node* getcloudrootnode()
    {
        return client.nodebyhandle(client.rootnodes[0]);
    }

    Node* gettestbasenode()
    {
        return client.childnodebyname(getcloudrootnode(), "mega_test_sync", false);
    }

    Node* getcloudrubbishnode()
    {
        return client.nodebyhandle(client.rootnodes[RUBBISHNODE - ROOTNODE]);
    }

    Node* drillchildnodebyname(Node* n, const string& path)
    {
        for (size_t p = 0; n && p < path.size(); )
        {
            auto pos = path.find("/", p);
            if (pos == string::npos) pos = path.size();
            n = client.childnodebyname(n, path.substr(p, pos - p).c_str(), false);
            p = pos == string::npos ? path.size() : pos + 1;
        }
        return n;
    }

    vector<Node*> drillchildnodesbyname(Node* n, const string& path)
    {
        auto pos = path.find("/");
        if (pos == string::npos)
        {
            return client.childnodesbyname(n, path.c_str(), false);
        }
        else
        {
            vector<Node*> results, subnodes = client.childnodesbyname(n, path.c_str(), false);
            for (size_t i = subnodes.size(); i--; )
            {
                if (subnodes[i]->type != FILENODE)
                {
                    vector<Node*> v = drillchildnodesbyname(subnodes[i], path.substr(pos + 1));
                    results.insert(results.end(), v.begin(), v.end());
                }
            }
            return results;
        }
    }

    bool setupSync_inthread(const string& subfoldername, const fs::path& localpath,
                            SyncCompletionFunction addSyncCompletion)
    {
        if (Node* n = client.nodebyhandle(basefolderhandle))
        {
            if (Node* m = drillchildnodebyname(n, subfoldername))
            {
                SyncConfig syncConfig{LocalPath::fromPath(localpath.u8string(), *client.fsaccess), localpath.u8string(), m->nodehandle, subfoldername, 0};
                error e = client.addsync(syncConfig, true, addSyncCompletion);
                return !e;
            }
        }
        return false;
    }

    bool delSync_inthread(handle backupId, const bool keepCache)
    {
        const auto handle = syncSet(backupId).h;
        bool removed = false;

        client.syncs.removeSelectedSyncs(
          [&](SyncConfig& c, Sync*)
          {
              const bool matched = c.getRemoteNode() == handle;

              removed |= matched;

              return matched;
          });

        return removed;
    }

    bool recursiveConfirm(Model::ModelNode* mn, Node* n, int& descendants, const string& identifier, int depth, bool& firstreported)
    {
        // top level names can differ so we don't check those
        if (!mn || !n) return false;
        if (depth && mn->name != n->displayname())
        {
            out() << "Node name mismatch: " << mn->path() << " " << n->displaypath() << endl;
            return false;
        }
        if (!mn->typematchesnodetype(n->type))
        {
            out() << "Node type mismatch: " << mn->path() << ":" << mn->type << " " << n->displaypath() << ":" << n->type << endl;
            return false;
        }

        if (n->type == FILENODE)
        {
            // not comparing any file versioning (for now)
            return true;
        }

        multimap<string, Model::ModelNode*> ms;
        multimap<string, Node*> ns;
        for (auto& m : mn->kids) ms.emplace(m->name, m.get());
        for (auto& n2 : n->children) ns.emplace(n2->displayname(), n2);

        int matched = 0;
        vector<string> matchedlist;
        for (auto m_iter = ms.begin(); m_iter != ms.end(); )
        {
            if (!depth && m_iter->first == DEBRISFOLDER)
            {
                m_iter = ms.erase(m_iter); // todo: add checks of the remote debris folder later
                continue;
            }

            auto er = ns.equal_range(m_iter->first);
            auto next_m = m_iter;
            ++next_m;
            bool any_equal_matched = false;
            for (auto i = er.first; i != er.second; ++i)
            {
                int rdescendants = 0;
                if (recursiveConfirm(m_iter->second, i->second, rdescendants, identifier, depth+1, firstreported))
                {
                    ++matched;
                    matchedlist.push_back(m_iter->first);
                    ns.erase(i);
                    ms.erase(m_iter);
                    descendants += rdescendants;
                    any_equal_matched = true;
                    break;
                }
            }
            if (!any_equal_matched)
            {
                break;
            }
            m_iter = next_m;
        }
        if (ns.empty() && ms.empty())
        {
            descendants += matched;
            return true;
        }
        else if (!firstreported)
        {
            firstreported = true;
            out() << clientname << " " << identifier << " after matching " << matched << " child nodes [";
            for (auto& ml : matchedlist) out() << ml << " ";
            out() << "](with " << descendants << " descendants) in " << mn->path() << ", ended up with unmatched model nodes:";
            for (auto& m : ms) out() << " " << m.first;
            out() << " and unmatched remote nodes:";
            for (auto& i : ns) out() << " " << i.first;
            out() << endl;
        };
        return false;
    }

    bool localNodesMustHaveNodes = true;

    bool recursiveConfirm(Model::ModelNode* mn, LocalNode* n, int& descendants, const string& identifier, int depth, bool& firstreported)
    {
        // top level names can differ so we don't check those
        if (!mn || !n) return false;
        if (depth && mn->name != n->name)
        {
            out() << "LocalNode name mismatch: " << mn->path() << " " << n->name << endl;
            return false;
        }
        if (!mn->typematchesnodetype(n->type))
        {
            out() << "LocalNode type mismatch: " << mn->path() << ":" << mn->type << " " << n->name << ":" << n->type << endl;
            return false;
        }

        auto localpath = n->getLocalPath().toName(*client.fsaccess, FS_UNKNOWN);
        string n_localname = n->localname.toName(*client.fsaccess, FS_UNKNOWN);
        if (n_localname.size())
        {
            EXPECT_EQ(n->name, n_localname);
        }
        if (localNodesMustHaveNodes)
        {
            EXPECT_TRUE(n->node != nullptr);
        }
        if (depth && n->node)
        {
            EXPECT_EQ(n->node->displayname(), n->name);
        }
        if (depth && mn->parent)
        {
            EXPECT_EQ(mn->parent->type, Model::ModelNode::folder);
            EXPECT_EQ(n->parent->type, FOLDERNODE);

            string parentpath = n->parent->getLocalPath().toName(*client.fsaccess, FS_UNKNOWN);
            EXPECT_EQ(localpath.substr(0, parentpath.size()), parentpath);
        }
        if (n->node && n->parent && n->parent->node)
        {
            string p = n->node->displaypath();
            string pp = n->parent->node->displaypath();
            EXPECT_EQ(p.substr(0, pp.size()), pp);
            EXPECT_EQ(n->parent->node, n->node->parent);
        }

        multimap<string, Model::ModelNode*> ms;
        multimap<string, LocalNode*> ns;
        for (auto& m : mn->kids)
        {
            ms.emplace(m->name, m.get());
        }
        for (auto& n2 : n->children)
        {
            if (!n2.second->deleted) ns.emplace(n2.second->name, n2.second); // todo: should LocalNodes marked as deleted actually have been removed by now?
        }

        int matched = 0;
        vector<string> matchedlist;
        for (auto m_iter = ms.begin(); m_iter != ms.end(); )
        {
            if (!depth && m_iter->first == DEBRISFOLDER)
            {
                m_iter = ms.erase(m_iter); // todo: are there LocalNodes representing the trash?
                continue;
            }

            auto er = ns.equal_range(m_iter->first);
            auto next_m = m_iter;
            ++next_m;
            bool any_equal_matched = false;
            for (auto i = er.first; i != er.second; ++i)
            {
                int rdescendants = 0;
                if (recursiveConfirm(m_iter->second, i->second, rdescendants, identifier, depth+1, firstreported))
                {
                    ++matched;
                    matchedlist.push_back(m_iter->first);
                    ns.erase(i);
                    ms.erase(m_iter);
                    descendants += rdescendants;
                    any_equal_matched = true;
                    break;
                }
            }
            if (!any_equal_matched)
            {
                break;
            }
            m_iter = next_m;
        }
        if (ns.empty() && ms.empty())
        {
            return true;
        }
        else if (!firstreported)
        {
            firstreported = true;
            out() << clientname << " " << identifier << " after matching " << matched << " child nodes [";
            for (auto& ml : matchedlist) out() << ml << " ";
            out() << "](with " << descendants << " descendants) in " << mn->path() << ", ended up with unmatched model nodes:";
            for (auto& m : ms) out() << " " << m.first;
            out() << " and unmatched LocalNodes:";
            for (auto& i : ns) out() << " " << i.first;
            out() << endl;
        };
        return false;
    }


    bool recursiveConfirm(Model::ModelNode* mn, fs::path p, int& descendants, const string& identifier, int depth, bool ignoreDebris, bool& firstreported)
    {
        if (!mn) return false;
        if (depth && mn->name != p.filename().u8string())
        {
            out() << "filesystem name mismatch: " << mn->path() << " " << p << endl;
            return false;
        }
        nodetype_t pathtype = fs::is_directory(p) ? FOLDERNODE : fs::is_regular_file(p) ? FILENODE : TYPE_UNKNOWN;
        if (!mn->typematchesnodetype(pathtype))
        {
            out() << "Path type mismatch: " << mn->path() << ":" << mn->type << " " << p.u8string() << ":" << pathtype << endl;
            return false;
        }

        if (pathtype == FILENODE && p.filename().u8string() != "lock")
        {
            if (localFSFilesThatMayDiffer.find(p) == localFSFilesThatMayDiffer.end())
            {
                ifstream fs(p, ios::binary);
                std::vector<char> buffer;
                buffer.resize(mn->content.size() + 1024);
                fs.read(reinterpret_cast<char *>(buffer.data()), buffer.size());
                EXPECT_EQ(size_t(fs.gcount()), mn->content.size()) << " file is not expected size " << p;
                EXPECT_TRUE(!memcmp(buffer.data(), mn->content.data(), mn->content.size())) << " file data mismatch " << p;
            }
        }

        if (pathtype != FOLDERNODE)
        {
            return true;
        }

        multimap<string, Model::ModelNode*> ms;
        multimap<string, fs::path> ps;
        for (auto& m : mn->kids) ms.emplace(m->name, m.get());
        for (fs::directory_iterator pi(p); pi != fs::directory_iterator(); ++pi) ps.emplace(pi->path().filename().u8string(), pi->path());

        if (ignoreDebris)
        {
            ps.erase(DEBRISFOLDER);
        }

        int matched = 0;
        vector<string> matchedlist;
        for (auto m_iter = ms.begin(); m_iter != ms.end(); )
        {
            auto er = ps.equal_range(m_iter->first);
            auto next_m = m_iter;
            ++next_m;
            bool any_equal_matched = false;
            for (auto i = er.first; i != er.second; ++i)
            {
                int rdescendants = 0;
                if (recursiveConfirm(m_iter->second, i->second, rdescendants, identifier, depth+1, ignoreDebris, firstreported))
                {
                    ++matched;
                    matchedlist.push_back(m_iter->first);
                    ps.erase(i);
                    ms.erase(m_iter);
                    descendants += rdescendants;
                    any_equal_matched = true;
                    break;
                }
            }
            if (!any_equal_matched)
            {
                break;
            }
            m_iter = next_m;
        }
        //if (ps.size() == 1 && !mn->parent && ps.begin()->first == DEBRISFOLDER)
        //{
        //    ps.clear();
        //}
        if (ps.empty() && ms.empty())
        {
            return true;
        }
        else if (!firstreported)
        {
            firstreported = true;
            out() << clientname << " " << identifier << " after matching " << matched << " child nodes [";
            for (auto& ml : matchedlist) out() << ml << " ";
            out() << "](with " << descendants << " descendants) in " << mn->path() << ", ended up with unmatched model nodes:";
            for (auto& m : ms) out() << " " << m.first;
            out() << " and unmatched filesystem paths:";
            for (auto& i : ps) out() << " " << i.second.filename();
            out() << " in " << p << endl;
        };
        return false;
    }

    Sync* syncByBackupId(handle backupId)
    {
        return client.syncs.runningSyncByBackupId(backupId);
    }

    enum Confirm
    {
        CONFIRM_LOCALFS = 0x01,
        CONFIRM_LOCALNODE = 0x02,
        CONFIRM_LOCAL = CONFIRM_LOCALFS | CONFIRM_LOCALNODE,
        CONFIRM_REMOTE = 0x04,
        CONFIRM_ALL = CONFIRM_LOCAL | CONFIRM_REMOTE,
    };

    bool confirmModel_mainthread(handle id, Model::ModelNode* mRoot, Node* rRoot)
    {
        auto result =
          thread_do<bool>(
            [=](StandardClient& client, PromiseBoolSP result)
            {
                result->set_value(client.confirmModel(id, mRoot, rRoot));
            });

        return result.get();
    }

    bool confirmModel_mainthread(handle id, Model::ModelNode* mRoot, LocalNode* lRoot)
    {
        auto result =
          thread_do<bool>(
            [=](StandardClient& client, PromiseBoolSP result)
            {
                result->set_value(client.confirmModel(id, mRoot, lRoot));
            });

        return result.get();
    }

    bool confirmModel_mainthread(handle id, Model::ModelNode* mRoot, fs::path lRoot, const bool ignoreDebris = false)
    {
        auto result =
          thread_do<bool>(
            [=](StandardClient& client, PromiseBoolSP result)
            {
                result->set_value(client.confirmModel(id, mRoot, lRoot, ignoreDebris));
            });

        return result.get();
    }

    bool confirmModel(handle id, Model::ModelNode* mRoot, Node* rRoot)
    {
        string name = "Sync " + toHandle(id);
        int descendents = 0;
        bool reported = false;

        if (!recursiveConfirm(mRoot, rRoot, descendents, name, 0, reported))
        {
            out() << clientname << " syncid " << toHandle(id) << " comparison against remote nodes failed" << endl;
            return false;
        }

        return true;
    }

    bool confirmModel(handle id, Model::ModelNode* mRoot, LocalNode* lRoot)
    {
        string name = "Sync " + toHandle(id);
        int descendents = 0;
        bool reported = false;

        if (!recursiveConfirm(mRoot, lRoot, descendents, name, 0, reported))
        {
            out() << clientname << " syncid " << toHandle(id) << " comparison against LocalNodes failed" << endl;
            return false;
        }

        return true;
    }

    bool confirmModel(handle id, Model::ModelNode* mRoot, fs::path lRoot, const bool ignoreDebris = false)
    {
        string name = "Sync " + toHandle(id);
        int descendents = 0;
        bool reported = false;

        if (!recursiveConfirm(mRoot, lRoot, descendents, name, 0, ignoreDebris, reported))
        {
            out() << clientname << " syncid " << toHandle(id) << " comparison against local filesystem failed" << endl;
            return false;
        }

        return true;
    }

    bool confirmModel(handle backupId, Model::ModelNode* mnode, const int confirm, const bool ignoreDebris)
    {
        SyncInfo si;

        if (!syncSet(backupId, si))
        {
            out() << clientname << " backupId " << toHandle(backupId) << " not found " << endl;
            return false;
        }

        // compare model against nodes representing remote state
        if ((confirm & CONFIRM_REMOTE) && !confirmModel(backupId, mnode, client.nodebyhandle(si.h)))
        {
            return false;
        }

        // compare model against LocalNodes
        if (Sync* sync = syncByBackupId(backupId))
        {
            if ((confirm & CONFIRM_LOCALNODE) && !confirmModel(backupId, mnode, sync->localroot.get()))
            {
                return false;
            }
        }

        // compare model against local filesystem
        if ((confirm & CONFIRM_LOCALFS) && !confirmModel(backupId, mnode, si.localpath, ignoreDebris))
        {
            return false;
        }

        return true;
    }

    void prelogin_result(int, string*, string* salt, error e) override
    {
        out() << clientname << " Prelogin: " << e << endl;
        if (!e)
        {
            this->salt = *salt;
        }
        resultproc.processresult(PRELOGIN, e, UNDEF);
    }

    void login_result(error e) override
    {
        out() << clientname << " Login: " << e << endl;
        resultproc.processresult(LOGIN, e, UNDEF);
    }

    void fetchnodes_result(const Error& e) override
    {
        out() << clientname << " Fetchnodes: " << e << endl;
        resultproc.processresult(FETCHNODES, e, UNDEF);
    }

    void unlink_result(handle, error e) override
    {
        resultproc.processresult(UNLINK, e, UNDEF);
    }

    void catchup_result() override
    {
        resultproc.processresult(CATCHUP, error(API_OK));
    }

    void putnodes_result(const Error& e, targettype_t tt, vector<NewNode>& nn, bool targetOverride) override
    {
        resultproc.processresult(PUTNODES, e, UNDEF);
    }

    void rename_result(handle h, error e)  override
    {
        resultproc.processresult(MOVENODE, e, h);
    }

    void deleteremote(string path, PromiseBoolSP pb)
    {
        if (Node* n = drillchildnodebyname(gettestbasenode(), path))
        {
            auto f = [pb](handle h, error e){ pb->set_value(!e); }; // todo: probably need better lifetime management for the promise, so multiple things can be tracked at once
            resultproc.prepresult(UNLINK, ++next_request_tag,
                [&](){ client.unlink(n, false, 0, f); },
                [pb](error e) { pb->set_value(!e); return true; });
        }
        else
        {
            pb->set_value(false);
        }
    }

    void deleteremotenodes(vector<Node*> ns, PromiseBoolSP pb)
    {
        if (ns.empty())
        {
            pb->set_value(true);
        }
        else
        {
            for (size_t i = ns.size(); i--; )
            {
                resultproc.prepresult(UNLINK, ++next_request_tag,
                    [&](){ client.unlink(ns[i], false, client.reqtag); },
                    [pb, i](error e) { if (!i) pb->set_value(!e); return true; });
            }
        }
    }

    void movenode(string path, string newparentpath, PromiseBoolSP pb)
    {
        Node* n = drillchildnodebyname(gettestbasenode(), path);
        Node* p = drillchildnodebyname(gettestbasenode(), newparentpath);
        if (n && p)
        {
            resultproc.prepresult(MOVENODE, ++next_request_tag,
                [&](){ client.rename(n, p); },
                [pb](error e) { pb->set_value(!e); return true; });
            return;
        }
        out() << "node or new parent not found" << endl;
        pb->set_value(false);
    }

    void movenode(handle h1, handle h2, PromiseBoolSP pb)
    {
        Node* n = client.nodebyhandle(h1);
        Node* p = client.nodebyhandle(h2);
        if (n && p)
        {
            resultproc.prepresult(MOVENODE, ++next_request_tag,
                [&](){ client.rename(n, p);},
                [pb](error e) { pb->set_value(!e); return true; });
            return;
        }
        out() << "node or new parent not found by handle" << endl;
        pb->set_value(false);
    }

    void movenodetotrash(string path, PromiseBoolSP pb)
    {
        Node* n = drillchildnodebyname(gettestbasenode(), path);
        Node* p = getcloudrubbishnode();
        if (n && p && n->parent)
        {
            resultproc.prepresult(MOVENODE, ++next_request_tag,
                [&](){ client.rename(n, p, SYNCDEL_NONE, n->parent->nodehandle); },
                [pb](error e) { pb->set_value(!e);  return true; });
            return;
        }
        out() << "node or rubbish or node parent not found" << endl;
        pb->set_value(false);
    }

    bool login_reset(const string& user, const string& pw, bool noCache = false)
    {
        future<bool> p1;
        p1 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.preloginFromEnv(user, pb); });
        if (!waitonresults(&p1))
        {
            out() << "preloginFromEnv failed" << endl;
            return false;
        }
        p1 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.loginFromEnv(user, pw, pb); });
        if (!waitonresults(&p1))
        {
            out() << "loginFromEnv failed" << endl;
            return false;
        }
        p1 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.fetchnodes(noCache, pb); });
        if (!waitonresults(&p1)) {
            out() << "fetchnodes failed" << endl;
            return false;
        }
        p1 = thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb) { sc.deleteTestBaseFolder(true, pb); });  // todo: do we need to wait for server response now
        if (!waitonresults(&p1)) {
            out() << "deleteTestBaseFolder failed" << endl;
            return false;
        }
        p1 = thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb) { sc.ensureTestBaseFolder(true, pb); });
        if (!waitonresults(&p1)) {
            out() << "ensureTestBaseFolder failed" << endl;
            return false;
        }
        return true;
    }

    bool login_reset_makeremotenodes(const string& user, const string& pw, const string& prefix, int depth, int fanout, bool noCache = false)
    {
        if (!login_reset(user, pw, noCache))
        {
            out() << "login_reset failed" << endl;
            return false;
        }
        future<bool> p1 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.makeCloudSubdirs(prefix, depth, fanout, pb); });
        if (!waitonresults(&p1))
        {
            out() << "makeCloudSubdirs failed" << endl;
            return false;
        }
        return true;
    }

    bool login_fetchnodes(const string& user, const string& pw, bool makeBaseFolder = false, bool noCache = false)
    {
        future<bool> p2;
        p2 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.preloginFromEnv(user, pb); });
        if (!waitonresults(&p2)) return false;
        p2 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.loginFromEnv(user, pw, pb); });
        if (!waitonresults(&p2)) return false;
        p2 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.fetchnodes(noCache, pb); });
        if (!waitonresults(&p2)) return false;
        p2 = thread_do<bool>([makeBaseFolder](StandardClient& sc, PromiseBoolSP pb) { sc.ensureTestBaseFolder(makeBaseFolder, pb); });
        if (!waitonresults(&p2)) return false;
        return true;
    }

    bool login_fetchnodes(const string& session)
    {
        future<bool> p2;
        p2 = thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.loginFromSession(session, pb); });
        if (!waitonresults(&p2)) return false;
        p2 = thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb) { sc.fetchnodes(false, pb); });
        if (!waitonresults(&p2)) return false;
        p2 = thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb) { sc.ensureTestBaseFolder(false, pb); });
        if (!waitonresults(&p2)) return false;
        return true;
    }

    //bool setupSync_mainthread(const std::string& localsyncrootfolder, const std::string& remotesyncrootfolder, handle syncid)
    //{
    //    //SyncConfig config{(fsBasePath / fs::u8path(localsyncrootfolder)).u8string(), drillchildnodebyname(gettestbasenode(), remotesyncrootfolder)->nodehandle, 0};
    //    return setupSync_mainthread(localsyncrootfolder, remotesyncrootfolder, syncid);
    //}

    handle setupSync_mainthread(const std::string& localsyncrootfolder, const std::string& remotesyncrootfolder)
    {
        fs::path syncdir = fsBasePath / fs::u8path(localsyncrootfolder);
        fs::create_directory(syncdir);
        auto fb = thread_do<handle>([=](StandardClient& mc, std::shared_ptr<promise<handle>> pb)
            {
                mc.setupSync_inthread(remotesyncrootfolder, syncdir,
                    [pb](UnifiedSync* us, const SyncError& se, error e)
                    {
                        pb->set_value(us != nullptr && !e && !se ? us->mConfig.getBackupId() : UNDEF);
                    });
            });
        return fb.get();
    }

    bool delSync_mainthread(handle backupId, bool keepCache = false)
    {
        future<bool> fb = thread_do<bool>([=](StandardClient& mc, PromiseBoolSP pb) { pb->set_value(mc.delSync_inthread(backupId, keepCache)); });
        return fb.get();
    }

    bool confirmModel_mainthread(Model::ModelNode* mnode, handle backupId, const bool ignoreDebris = false, const int confirm = CONFIRM_ALL)
    {
        future<bool> fb;
        fb = thread_do<bool>([backupId, mnode, ignoreDebris, confirm](StandardClient& sc, PromiseBoolSP pb) { pb->set_value(sc.confirmModel(backupId, mnode, confirm, ignoreDebris)); });
        return fb.get();
    }

};


void waitonsyncs(chrono::seconds d = std::chrono::seconds(4), StandardClient* c1 = nullptr, StandardClient* c2 = nullptr, StandardClient* c3 = nullptr, StandardClient* c4 = nullptr)
{
    auto totalTimeoutStart = chrono::steady_clock::now();
    auto start = chrono::steady_clock::now();
    std::vector<StandardClient*> v{ c1, c2, c3, c4 };
    bool onelastsyncdown = true;
    for (;;)
    {
        bool any_add_del = false;

        for (auto vn : v)
        {
            if (vn)
            {
                auto result =
                  vn->thread_do<bool>(
                    [&](StandardClient& mc, PromiseBoolSP result)
                    {
                        bool busy = false;

                        mc.client.syncs.forEachRunningSync(
                          [&](Sync* s)
                          {
                              busy |= !s->deleteq.empty();
                              busy |= !s->insertq.empty();
                          });

                        if (!(mc.client.todebris.empty()
                            && mc.client.localsyncnotseen.empty()
                            && mc.client.tounlink.empty()
                            && mc.client.synccreate.empty()
                            && mc.client.transferlist.transfers[GET].empty()
                            && mc.client.transferlist.transfers[PUT].empty()))
                        {
                            busy = true;
                        }

                        result->set_value(busy);
                    });

                any_add_del |= result.get();
            }
        }

        bool allactive = true;
        {
            //lock_guard<mutex> g(StandardClient::om);
            //out() << "sync state: ";
            //for (auto n : syncstates)
            //{
            //    cout << n;
            //    if (n != SYNC_ACTIVE) allactive = false;
            //}
            //out() << endl;
        }

        if (any_add_del || StandardClient::debugging)
        {
            start = chrono::steady_clock::now();
        }

        if (onelastsyncdown && (chrono::steady_clock::now() - start + d/2) > d)
        {
            // synced folders that were removed remotely don't have the corresponding local folder removed unless we prompt an extra syncdown.  // todo:  do we need to fix
            for (auto vn : v) if (vn) vn->client.syncdownrequired = true;
            onelastsyncdown = false;
        }

        for (auto vn : v) if (vn)
        {
            if (allactive && ((chrono::steady_clock::now() - start) > d) && ((chrono::steady_clock::now() - vn->lastcb) > d))
            {
                return;
            }
        }

        WaitMillisec(400);

        if ((chrono::steady_clock::now() - totalTimeoutStart) > std::chrono::minutes(5))
        {
            out() << "Waiting for syncing to stop timed out at 5 minutes" << endl;
            return;
        }
    }

}


mutex StandardClient::om;
bool StandardClient::debugging = false;



//std::atomic<int> fileSizeCount = 20;

bool createNameFile(const fs::path &p, const string &filename)
{
    return createFile(p / fs::u8path(filename), filename.data(), filename.size());
}

bool createDataFileWithTimestamp(const fs::path &path,
                             const std::string &data,
                             const fs::file_time_type &timestamp)
{
    const bool result = createDataFile(path, data);

    if (result)
    {
        fs::last_write_time(path, timestamp);
    }

    return result;
}

bool buildLocalFolders(fs::path targetfolder, const string& prefix, int n, int recurselevel, int filesperfolder)
{
    if (suppressfiles) filesperfolder = 0;

    fs::path p = targetfolder / fs::u8path(prefix);
    if (!fs::create_directory(p))
        return false;

    for (int i = 0; i < filesperfolder; ++i)
    {
        string filename = "file" + to_string(i) + "_" + prefix;
        createNameFile(p, filename);
        //int thisSize = (++fileSizeCount)/2;
        //for (int j = 0; j < thisSize; ++j) fs << ('0' + j % 10);
    }

    if (recurselevel > 0)
    {
        for (int i = 0; i < n; ++i)
        {
            if (!buildLocalFolders(p, prefix + "_" + to_string(i), n, recurselevel - 1, filesperfolder))
                return false;
        }
    }

    return true;
}

void renameLocalFolders(fs::path targetfolder, const string& newprefix)
{
    std::list<fs::path> toRename;
    for (fs::directory_iterator i(targetfolder); i != fs::directory_iterator(); ++i)
    {
        if (fs::is_directory(i->path()))
        {
            renameLocalFolders(i->path(), newprefix);
        }
        toRename.push_back(i->path());
    }

    for (auto p : toRename)
    {
        auto newpath = p.parent_path() / (newprefix + p.filename().u8string());
        fs::rename(p, newpath);
    }
}


#ifdef __linux__
bool createSpecialFiles(fs::path targetfolder, const string& prefix, int n = 1)
{
    fs::path p = targetfolder;
    for (int i = 0; i < n; ++i)
    {
        string filename = "file" + to_string(i) + "_" + prefix;
        fs::path fp = p / fs::u8path(filename);

        int fdtmp = openat(AT_FDCWD, p.c_str(), O_RDWR|O_CLOEXEC|O_TMPFILE, 0600);
        write(fdtmp, filename.data(), filename.size());

        stringstream fdproc;
        fdproc << "/proc/self/fd/";
        fdproc << fdtmp;

        int r = linkat(AT_FDCWD, fdproc.str().c_str() , AT_FDCWD, fp.c_str(), AT_SYMLINK_FOLLOW);
        if (r)
        {
            cerr << " errno =" << errno << endl;
            return false;
        }
        close(fdtmp);
    }
    return true;
}
#endif

} // anonymous

class SyncFingerprintCollision
  : public ::testing::Test
{
public:
    SyncFingerprintCollision()
      : client0()
      , client1()
      , model0()
      , model1()
      , arbitraryFileLength(16384)
    {
        const fs::path root = makeNewTestRoot();

        client0 = ::mega::make_unique<StandardClient>(root, "c0");
        client1 = ::mega::make_unique<StandardClient>(root, "c1");

        client0->logcb = true;
        client1->logcb = true;
    }

    ~SyncFingerprintCollision()
    {
    }

    void SetUp() override
    {
        ASSERT_TRUE(client0->login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "d", 1, 2));
        ASSERT_TRUE(client1->login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
        ASSERT_EQ(client0->basefolderhandle, client1->basefolderhandle);

        model0.root->addkid(model0.buildModelSubdirs("d", 2, 1, 0));
        model1.root->addkid(model1.buildModelSubdirs("d", 2, 1, 0));

        startSyncs();
        waitOnSyncs();
        confirmModels();
    }

    void addModelFile(Model &model,
                      const std::string &directory,
                      const std::string &file,
                      const std::string &content)
    {
        auto *node = model.findnode(directory);
        ASSERT_NE(node, nullptr);

        node->addkid(model.makeModelSubfile(file, content));
    }

    void confirmModel(StandardClient &client, Model &model, handle backupId)
    {
        ASSERT_TRUE(client.confirmModel_mainthread(model.findnode("d"), backupId));
    }

    void confirmModels()
    {
        confirmModel(*client0, model0, backupId0);
        confirmModel(*client1, model1, backupId1);
    }

    const fs::path localRoot0() const
    {
        return client0->syncSet(backupId0).localpath;
    }

    const fs::path localRoot1() const
    {
        return client1->syncSet(backupId1).localpath;
    }

    std::string randomData(const std::size_t length) const
    {
        std::vector<uint8_t> data(length);

        std::generate_n(data.begin(), data.size(), [](){ return (uint8_t)std::rand(); });

        return std::string((const char*)data.data(), data.size());
    }

    void startSyncs()
    {
        backupId0 = client0->setupSync_mainthread("s0", "d");
        ASSERT_NE(backupId0, UNDEF);
        backupId1 = client1->setupSync_mainthread("s1", "d");
        ASSERT_NE(backupId1, UNDEF);
    }

    void waitOnSyncs()
    {
        waitonsyncs(chrono::seconds(4), client0.get(), client1.get());
    }

    handle backupId0 = UNDEF;
    handle backupId1 = UNDEF;

    std::unique_ptr<StandardClient> client0;
    std::unique_ptr<StandardClient> client1;
    Model model0;
    Model model1;
    const std::size_t arbitraryFileLength;
}; /* SyncFingerprintCollision */

TEST_F(SyncFingerprintCollision, DifferentMacSameName)
{
    auto data0 = randomData(arbitraryFileLength);
    auto data1 = data0;
    const auto path0 = localRoot0() / "d_0" / "a";
    const auto path1 = localRoot0() / "d_1" / "a";

    // Alter MAC but leave fingerprint untouched.
    data1[0x41] = static_cast<uint8_t>(~data1[0x41]);

    ASSERT_TRUE(createDataFile(path0, data0));
    waitOnSyncs();

    auto result0 =
      client0->thread_do<bool>([&](StandardClient &sc, PromiseBoolSP p)
                         {
                             p->set_value(
                                 createDataFileWithTimestamp(
                                 path1,
                                 data1,
                                 fs::last_write_time(path0)));
                         });

    ASSERT_TRUE(waitonresults(&result0));
    waitOnSyncs();

    addModelFile(model0, "d/d_0", "a", data0);
    addModelFile(model0, "d/d_1", "a", data1);
    addModelFile(model1, "d/d_0", "a", data0);
    addModelFile(model1, "d/d_1", "a", data0);
    model1.ensureLocalDebrisTmpLock("d");

    confirmModels();
}

TEST_F(SyncFingerprintCollision, DifferentMacDifferentName)
{
    auto data0 = randomData(arbitraryFileLength);
    auto data1 = data0;
    const auto path0 = localRoot0() / "d_0" / "a";
    const auto path1 = localRoot0() / "d_0" / "b";

    data1[0x41] = static_cast<uint8_t>(~data1[0x41]);

    ASSERT_TRUE(createDataFile(path0, data0));
    waitOnSyncs();

    auto result0 =
      client0->thread_do<bool>([&](StandardClient &sc, PromiseBoolSP p)
                         {
                             p->set_value(
                                 createDataFileWithTimestamp(
                                 path1,
                                 data1,
                                 fs::last_write_time(path0)));
                         });

    ASSERT_TRUE(waitonresults(&result0));
    waitOnSyncs();

    addModelFile(model0, "d/d_0", "a", data0);
    addModelFile(model0, "d/d_0", "b", data1);
    addModelFile(model1, "d/d_0", "a", data0);
    addModelFile(model1, "d/d_0", "b", data1);
    model1.ensureLocalDebrisTmpLock("d");

    confirmModels();
}

TEST_F(SyncFingerprintCollision, SameMacDifferentName)
{
    auto data0 = randomData(arbitraryFileLength);
    const auto path0 = localRoot0() / "d_0" / "a";
    const auto path1 = localRoot0() / "d_0" / "b";

    ASSERT_TRUE(createDataFile(path0, data0));
    waitOnSyncs();

    auto result0 =
      client0->thread_do<bool>([&](StandardClient &sc, PromiseBoolSP p)
                         {
                            p->set_value(
                                 createDataFileWithTimestamp(
                                 path1,
                                 data0,
                                 fs::last_write_time(path0)));
                         });

    ASSERT_TRUE(waitonresults(&result0));
    waitOnSyncs();

    addModelFile(model0, "d/d_0", "a", data0);
    addModelFile(model0, "d/d_0", "b", data0);
    addModelFile(model1, "d/d_0", "a", data0);
    addModelFile(model1, "d/d_0", "b", data0);
    model1.ensureLocalDebrisTmpLock("d");

    confirmModels();
}

GTEST_TEST(Sync, BasicSync_DelRemoteFolder)
{
    // delete a remote folder and confirm the client sending the request and another also synced both correctly update the disk
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2


    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    // delete something remotely and let sync catch up
    future<bool> fb = clientA1.thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb) { sc.deleteremote("f/f_2/f_2_1", pb); });
    ASSERT_TRUE(waitonresults(&fb));
    waitonsyncs(std::chrono::seconds(60), &clientA1, &clientA2);

    // check everything matches in both syncs (model has expected state of remote and local)
    ASSERT_TRUE(model.movetosynctrash("f/f_2/f_2_1", "f"));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
}

GTEST_TEST(Sync, BasicSync_DelLocalFolder)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    auto checkpath = clientA1.syncSet(backupId1).localpath.u8string();
    out() << "checking paths " << checkpath << '\n';
    LOG_debug << "checking paths" << checkpath;
    for(auto& p: fs::recursive_directory_iterator(TestFS::GetTestFolder()))
    {
        out() << "checking path is present: " << p.path().u8string() << '\n';
        LOG_debug << "checking path is present: " << p.path().u8string();
    }
    // delete something in the local filesystem and see if we catch up in A1 and A2 (deleter and observer syncs)
    error_code e;
    auto nRemoved = fs::remove_all(clientA1.syncSet(backupId1).localpath / "f_2" / "f_2_1", e);
    ASSERT_TRUE(!e) << "remove failed " << (clientA1.syncSet(backupId1).localpath / "f_2" / "f_2_1").u8string() << " error " << e;
    ASSERT_GT(nRemoved, 0) << e;

    // let them catch up
    waitonsyncs(std::chrono::seconds(20), &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(model.movetosynctrash("f/f_2/f_2_1", "f"));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
    ASSERT_TRUE(model.removesynctrash("f"));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
}

GTEST_TEST(Sync, BasicSync_MoveLocalFolder)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    // move something in the local filesystem and see if we catch up in A1 and A2 (deleter and observer syncs)
    error_code rename_error;
    fs::rename(clientA1.syncSet(backupId1).localpath / "f_2" / "f_2_1", clientA1.syncSet(backupId1).localpath / "f_2_1", rename_error);
    ASSERT_TRUE(!rename_error) << rename_error;

    // let them catch up
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(model.movenode("f/f_2/f_2_1", "f"));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
}

GTEST_TEST(Sync, BasicSync_MoveLocalFolderBetweenSyncs)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2
    StandardClient clientA3(localtestroot, "clientA3");   // user 1 client 3

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_TRUE(clientA3.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    // set up sync for A1 and A2, it should build matching local folders
    handle backupId11 = clientA1.setupSync_mainthread("sync1", "f/f_0");
    ASSERT_NE(backupId11, UNDEF);
    handle backupId12 = clientA1.setupSync_mainthread("sync2", "f/f_2");
    ASSERT_NE(backupId12, UNDEF);
    handle backupId21 = clientA2.setupSync_mainthread("syncA2_1", "f/f_0");
    ASSERT_NE(backupId21, UNDEF);
    handle backupId22 = clientA2.setupSync_mainthread("syncA2_2", "f/f_2");
    ASSERT_NE(backupId22, UNDEF);
    handle backupId31 = clientA3.setupSync_mainthread("syncA3", "f");
    ASSERT_NE(backupId31, UNDEF);
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2, &clientA3);
    clientA1.logcb = clientA2.logcb = clientA3.logcb = true;

    // check everything matches (model has expected state of remote and local)
    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f/f_0"), backupId11));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f/f_2"), backupId12));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f/f_0"), backupId21));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f/f_2"), backupId22));
    ASSERT_TRUE(clientA3.confirmModel_mainthread(model.findnode("f"), backupId31));

    // move a folder form one local synced folder to another local synced folder and see if we sync correctly and catch up in A2 and A3 (mover and observer syncs)
    error_code rename_error;
    fs::path path1 = clientA1.syncSet(backupId11).localpath / "f_0_1";
    fs::path path2 = clientA1.syncSet(backupId12).localpath / "f_2_1" / "f_2_1_0" / "f_0_1";
    fs::rename(path1, path2, rename_error);
    ASSERT_TRUE(!rename_error) << rename_error;

    // let them catch up
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2, &clientA3);

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(model.movenode("f/f_0/f_0_1", "f/f_2/f_2_1/f_2_1_0"));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f/f_0"), backupId11));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f/f_2"), backupId12));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f/f_0"), backupId21));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f/f_2"), backupId22));
    ASSERT_TRUE(clientA3.confirmModel_mainthread(model.findnode("f"), backupId31));
}

GTEST_TEST(Sync, BasicSync_RenameLocalFile)
{
    static auto TIMEOUT = std::chrono::seconds(4);

    const fs::path root = makeNewTestRoot();

    // Primary client.
    StandardClient client0(root, "c0");
    // Observer.
    StandardClient client1(root, "c1");

    // Log callbacks.
    client0.logcb = true;
    client1.logcb = true;

    // Log clients in.
    ASSERT_TRUE(client0.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "x", 0, 0));
    ASSERT_TRUE(client1.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(client0.basefolderhandle, client1.basefolderhandle);

    // Set up syncs.
    handle backupId0 = client0.setupSync_mainthread("s0", "x");
    ASSERT_NE(backupId0, UNDEF);
    handle backupId1 = client1.setupSync_mainthread("s1", "x");
    ASSERT_NE(backupId1, UNDEF);

    // Wait for initial sync to complete.
    waitonsyncs(TIMEOUT, &client0, &client1);

    // Add x/f.
    ASSERT_TRUE(createNameFile(client0.syncSet(backupId0).localpath, "f"));

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, &client0, &client1);

    // Confirm model.
    Model model;

    model.root->addkid(model.makeModelSubfolder("x"));
    model.findnode("x")->addkid(model.makeModelSubfile("f"));

    ASSERT_TRUE(client0.confirmModel_mainthread(model.findnode("x"), backupId0));
    ASSERT_TRUE(client1.confirmModel_mainthread(model.findnode("x"), backupId1, true));

    // Rename x/f to x/g.
    fs::rename(client0.syncSet(backupId0).localpath / "f",
               client0.syncSet(backupId0).localpath / "g");

    // Wait for sync to complete.
    waitonsyncs(TIMEOUT, &client0, &client1);

    // Update and confirm model.
    model.findnode("x/f")->name = "g";

    ASSERT_TRUE(client0.confirmModel_mainthread(model.findnode("x"), backupId0));
    ASSERT_TRUE(client1.confirmModel_mainthread(model.findnode("x"), backupId1, true));
}

GTEST_TEST(Sync, BasicSync_AddLocalFolder)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    // make new folders (and files) in the local filesystem and see if we catch up in A1 and A2 (adder and observer syncs)
    ASSERT_TRUE(buildLocalFolders(clientA1.syncSet(backupId1).localpath / "f_2", "newkid", 2, 2, 2));

    // let them catch up
    waitonsyncs(std::chrono::seconds(30), &clientA1, &clientA2);  // two minutes should be long enough to get past API_ETEMPUNAVAIL == -18 for sync2 downloading the files uploaded by sync1

    // check everything matches (model has expected state of remote and local)
    model.findnode("f/f_2")->addkid(model.buildModelSubdirs("newkid", 2, 2, 2));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    model.ensureLocalDebrisTmpLock("f"); // since we downloaded files
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
}


// todo: add this test once the sync can keep up with file system notifications - at the moment
// it's too slow because we wait for the cloud before processing the next layer of files+folders.
// So if we add enough changes to exercise the notification queue, we can't check the results because
// it's far too slow at the syncing stage.
GTEST_TEST(Sync, BasicSync_MassNotifyFromLocalFolderTree)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    //StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 0, 0));
    //ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    //ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    //ASSERT_TRUE(clientA2.setupSync_mainthread("sync2", "f", 2));
    waitonsyncs(std::chrono::seconds(4), &clientA1/*, &clientA2*/);
    //clientA1.logcb = clientA2.logcb = true;

    // Create a directory tree in one sync, it should be synced to the cloud and back to the other
    // Create enough files and folders that we put a strain on the notification logic: 3k entries
    ASSERT_TRUE(buildLocalFolders(clientA1.syncSet(backupId1).localpath, "initial", 0, 0, 16000));

    //waitonsyncs(std::chrono::seconds(10), &clientA1 /*, &clientA2*/);
    std::this_thread::sleep_for(std::chrono::seconds(5));

    // wait until the notify queues subside, it shouldn't take too long.  Limit of 5 minutes
    auto startTime = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - startTime < std::chrono::seconds(5 * 60))
    {
        size_t remaining = 0;
        auto result0 = clientA1.thread_do<bool>([&](StandardClient &sc, PromiseBoolSP p)
        {
            sc.client.syncs.forEachRunningSync(
              [&](Sync* s)
              {
                  for (int q = DirNotify::NUMQUEUES; q--; )
                  {
                      remaining += s->dirnotify->notifyq[q].size();
                  }
              });

            p->set_value(true);
        });
        result0.get();
        if (!remaining) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    Model model;
    model.root->addkid(model.buildModelSubdirs("initial", 0, 0, 16000));

    // check everything matches (just local since it'll still be uploading files)
    clientA1.localNodesMustHaveNodes = false;
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.root.get(), backupId1, false, StandardClient::CONFIRM_LOCAL));
    //ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), 2));

    ASSERT_GT(clientA1.transfersAdded.load(), 0u);
    clientA1.transfersAdded = 0;

    // rename all those files and folders, put a strain on the notify system again.
    // Also, no downloads (or uploads) should occur as a result of this.
 //   renameLocalFolders(clientA1.syncSet(backupId1).localpath, "renamed_");

    // let them catch up
    //waitonsyncs(std::chrono::seconds(10), &clientA1 /*, &clientA2*/);

    // rename is too slow to check, even just in localnodes, for now.

    //ASSERT_EQ(clientA1.transfersAdded.load(), 0u);

    //Model model2;
    //model2.root->addkid(model.buildModelSubdirs("renamed_initial", 0, 0, 100));

    //// check everything matches (model has expected state of remote and local)
    //ASSERT_TRUE(clientA1.confirmModel_mainthread(model2.root.get(), 1));
    ////ASSERT_TRUE(clientA2.confirmModel_mainthread(model2.findnode("f"), 2));
}



/* this one is too slow for regular testing with the current algorithm
GTEST_TEST(Sync, BasicSync_MAX_NEWNODES1)
{
    // create more nodes than we can upload in one putnodes.
    // this tree is 5x5 and the algorithm ends up creating nodes one at a time so it's pretty slow (and doesn't hit MAX_NEWNODES as a result)
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // set up sync for A1, it should build matching local folders
    ASSERT_TRUE(clientA1.setupSync_mainthread("sync1", "f", 1));
    ASSERT_TRUE(clientA2.setupSync_mainthread("sync2", "f", 2));
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), 1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), 2));

    // make new folders in the local filesystem and see if we catch up in A1 and A2 (adder and observer syncs)
    assert(MegaClient::MAX_NEWNODES < 3125);
    ASSERT_TRUE(buildLocalFolders(clientA1.syncSet(backupId1).localpath, "g", 5, 5, 0));  // 5^5=3125 leaf folders, 625 pre-leaf etc

    // let them catch up
    waitonsyncs(std::chrono::seconds(30), &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    model.findnode("f")->addkid(model.buildModelSubdirs("g", 5, 5, 0));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), 1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), 2));
}
*/

/* this one is too slow for regular testing with the current algorithm
GTEST_TEST(Sync, BasicSync_MAX_NEWNODES2)
{
    // create more nodes than we can upload in one putnodes.
    // this tree is 5x5 and the algorithm ends up creating nodes one at a time so it's pretty slow (and doesn't hit MAX_NEWNODES as a result)
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // set up sync for A1, it should build matching local folders
    ASSERT_TRUE(clientA1.setupSync_mainthread("sync1", "f", 1));
    ASSERT_TRUE(clientA2.setupSync_mainthread("sync2", "f", 2));
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), 1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), 2));

    // make new folders in the local filesystem and see if we catch up in A1 and A2 (adder and observer syncs)
    assert(MegaClient::MAX_NEWNODES < 3000);
    ASSERT_TRUE(buildLocalFolders(clientA1.syncSet(backupId1).localpath, "g", 3000, 1, 0));

    // let them catch up
    waitonsyncs(std::chrono::seconds(30), &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    model.findnode("f")->addkid(model.buildModelSubdirs("g", 3000, 1, 0));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), 1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), 2));
}
*/

GTEST_TEST(Sync, BasicSync_MoveExistingIntoNewLocalFolder)
{
    // historic case:  in the local filesystem, create a new folder then move an existing file/folder into it
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    // make new folder in the local filesystem
    ASSERT_TRUE(buildLocalFolders(clientA1.syncSet(backupId1).localpath, "new", 1, 0, 0));
    // move an already synced folder into it
    error_code rename_error;
    fs::path path1 = clientA1.syncSet(backupId1).localpath / "f_2"; // / "f_2_0" / "f_2_0_0";
    fs::path path2 = clientA1.syncSet(backupId1).localpath / "new" / "f_2"; // "f_2_0_0";
    fs::rename(path1, path2, rename_error);
    ASSERT_TRUE(!rename_error) << rename_error;

    // let them catch up
    waitonsyncs(std::chrono::seconds(10), &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    auto f = model.makeModelSubfolder("new");
    f->addkid(model.removenode("f/f_2")); // / f_2_0 / f_2_0_0"));
    model.findnode("f")->addkid(move(f));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
}

GTEST_TEST(Sync, DISABLED_BasicSync_MoveSeveralExistingIntoDeepNewLocalFolders)
{
    // historic case:  in the local filesystem, create a new folder then move an existing file/folder into it
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    // make new folder tree in the local filesystem
    ASSERT_TRUE(buildLocalFolders(clientA1.syncSet(backupId1).localpath, "new", 3, 3, 3));

    // move already synced folders to serveral parts of it - one under another moved folder too
    error_code rename_error;
    fs::rename(clientA1.syncSet(backupId1).localpath / "f_0", clientA1.syncSet(backupId1).localpath / "new" / "new_0" / "new_0_1" / "new_0_1_2" / "f_0", rename_error);
    ASSERT_TRUE(!rename_error) << rename_error;
    fs::rename(clientA1.syncSet(backupId1).localpath / "f_1", clientA1.syncSet(backupId1).localpath / "new" / "new_1" / "new_1_2" / "f_1", rename_error);
    ASSERT_TRUE(!rename_error) << rename_error;
    fs::rename(clientA1.syncSet(backupId1).localpath / "f_2", clientA1.syncSet(backupId1).localpath / "new" / "new_1" / "new_1_2" / "f_1" / "f_1_2" / "f_2", rename_error);
    ASSERT_TRUE(!rename_error) << rename_error;

    // let them catch up
    waitonsyncs(std::chrono::seconds(30), &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    model.findnode("f")->addkid(model.buildModelSubdirs("new", 3, 3, 3));
    model.findnode("f/new/new_0/new_0_1/new_0_1_2")->addkid(model.removenode("f/f_0"));
    model.findnode("f/new/new_1/new_1_2")->addkid(model.removenode("f/f_1"));
    model.findnode("f/new/new_1/new_1_2/f_1/f_1_2")->addkid(model.removenode("f/f_2"));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    model.ensureLocalDebrisTmpLock("f"); // since we downloaded files
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
}

/* not expected to work yet
GTEST_TEST(Sync, BasicSync_SyncDuplicateNames)
{
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);


    NewNode* nodearray = new NewNode[3];
    nodearray[0] = *clientA1.makeSubfolder("samename");
    nodearray[1] = *clientA1.makeSubfolder("samename");
    nodearray[2] = *clientA1.makeSubfolder("Samename");
    clientA1.resultproc.prepresult(StandardClient::PUTNODES, [this](error e) {
    });
    clientA1.client.putnodes(clientA1.basefolderhandle, nodearray, 3);

    // set up syncs, they should build matching local folders
    ASSERT_TRUE(clientA1.setupSync_mainthread("sync1", "", 1));
    ASSERT_TRUE(clientA2.setupSync_mainthread("sync2", "", 2));
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    Model model;
    model.root->addkid(model.makeModelSubfolder("samename"));
    model.root->addkid(model.makeModelSubfolder("samename"));
    model.root->addkid(model.makeModelSubfolder("Samename"));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.root.get(), 1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.root.get(), 2));
}*/

GTEST_TEST(Sync, BasicSync_RemoveLocalNodeBeforeSessionResume)
{
    fs::path localtestroot = makeNewTestRoot();
    auto pclientA1 = ::mega::make_unique<StandardClient>(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(pclientA1->login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(pclientA1->basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = pclientA1->setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), pclientA1.get(), &clientA2);
    pclientA1->logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(pclientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    // save session
    string session;
    pclientA1->client.dumpsession(session);

    // logout (but keep caches)
    fs::path sync1path = pclientA1->syncSet(backupId1).localpath;
    pclientA1->localLogout();

    // remove local folders
    error_code e;
    ASSERT_TRUE(fs::remove_all(sync1path / "f_2", e) != static_cast<std::uintmax_t>(-1)) << e;

    // resume session, see if nodes and localnodes get in sync
    pclientA1.reset(new StandardClient(localtestroot, "clientA1"));
    ASSERT_TRUE(pclientA1->login_fetchnodes(session));

    waitonsyncs(std::chrono::seconds(4), pclientA1.get(), &clientA2);

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(model.movetosynctrash("f/f_2", "f"));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
    ASSERT_TRUE(model.removesynctrash("f"));
    ASSERT_TRUE(pclientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
}

/* not expected to work yet
GTEST_TEST(Sync, BasicSync_RemoteFolderCreationRaceSamename)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    // SN tagging needed for this one
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    // set up sync for both, it should build matching local folders (empty initially)
    ASSERT_TRUE(clientA1.setupSync_mainthread("sync1", "", 1));
    ASSERT_TRUE(clientA2.setupSync_mainthread("sync2", "", 2));
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // now have both clients create the same remote folder structure simultaneously.  We should end up with just one copy of it on the server and in both syncs
    future<bool> p1 = clientA1.thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.makeCloudSubdirs("f", 3, 3, pb); });
    future<bool> p2 = clientA2.thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { sc.makeCloudSubdirs("f", 3, 3, pb); });
    ASSERT_TRUE(waitonresults(&p1, &p2));

    // let them catch up
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.root.get(), 1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.root.get(), 2));
}*/

/* not expected to work yet
GTEST_TEST(Sync, BasicSync_LocalFolderCreationRaceSamename)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    // SN tagging needed for this one
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    // set up sync for both, it should build matching local folders (empty initially)
    ASSERT_TRUE(clientA1.setupSync_mainthread("sync1", "", 1));
    ASSERT_TRUE(clientA2.setupSync_mainthread("sync2", "", 2));
    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;

    // now have both clients create the same folder structure simultaneously.  We should end up with just one copy of it on the server and in both syncs
    future<bool> p1 = clientA1.thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { buildLocalFolders(sc.syncSet(backupId1).localpath, "f", 3, 3, 0); pb->set_value(true); });
    future<bool> p2 = clientA2.thread_do<bool>([=](StandardClient& sc, PromiseBoolSP pb) { buildLocalFolders(sc.syncSet(backupId2).localpath, "f", 3, 3, 0); pb->set_value(true); });
    ASSERT_TRUE(waitonresults(&p1, &p2));

    // let them catch up
    waitonsyncs(std::chrono::seconds(30), &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.root.get(), 1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.root.get(), 2));
}*/


GTEST_TEST(Sync, BasicSync_ResumeSyncFromSessionAfterNonclashingLocalAndRemoteChanges )
{
    fs::path localtestroot = makeNewTestRoot();
    unique_ptr<StandardClient> pclientA1(new StandardClient(localtestroot, "clientA1"));   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(pclientA1->login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(pclientA1->basefolderhandle, clientA2.basefolderhandle);

    // set up sync for A1, it should build matching local folders
    handle backupId1 = pclientA1->setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), pclientA1.get(), &clientA2);
    pclientA1->logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    Model model1, model2;
    model1.root->addkid(model1.buildModelSubdirs("f", 3, 3, 0));
    model2.root->addkid(model2.buildModelSubdirs("f", 3, 3, 0));
    ASSERT_TRUE(pclientA1->confirmModel_mainthread(model1.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model2.findnode("f"), backupId2));

    out() << "********************* save session A1" << endl;
    string session;
    pclientA1->client.dumpsession(session);

    out() << "*********************  logout A1 (but keep caches on disk)" << endl;
    fs::path sync1path = pclientA1->syncSet(backupId1).localpath;
    pclientA1->localLogout();

    out() << "*********************  add remote folders via A2" << endl;
    future<bool> p1 = clientA2.thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb) { sc.makeCloudSubdirs("newremote", 2, 2, pb, "f/f_1/f_1_0"); });
    model1.findnode("f/f_1/f_1_0")->addkid(model1.buildModelSubdirs("newremote", 2, 2, 0));
    model2.findnode("f/f_1/f_1_0")->addkid(model2.buildModelSubdirs("newremote", 2, 2, 0));
    ASSERT_TRUE(waitonresults(&p1));

    out() << "*********************  remove remote folders via A2" << endl;
    p1 = clientA2.thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb) { sc.deleteremote("f/f_0", pb); });
    model1.movetosynctrash("f/f_0", "f");
    model2.movetosynctrash("f/f_0", "f");
    ASSERT_TRUE(waitonresults(&p1));

    out() << "*********************  add local folders in A1" << endl;
    ASSERT_TRUE(buildLocalFolders(sync1path / "f_1/f_1_2", "newlocal", 2, 2, 2));
    model1.findnode("f/f_1/f_1_2")->addkid(model1.buildModelSubdirs("newlocal", 2, 2, 2));
    model2.findnode("f/f_1/f_1_2")->addkid(model2.buildModelSubdirs("newlocal", 2, 2, 2));

    out() << "*********************  remove local folders in A1" << endl;
    error_code e;
    ASSERT_TRUE(fs::remove_all(sync1path / "f_2", e) != static_cast<std::uintmax_t>(-1)) << e;
    model1.removenode("f/f_2");
    model2.movetosynctrash("f/f_2", "f");

    out() << "*********************  get sync2 activity out of the way" << endl;
    waitonsyncs(DEFAULTWAIT, &clientA2);

    out() << "*********************  resume A1 session (with sync), see if A2 nodes and localnodes get in sync again" << endl;
    pclientA1.reset(new StandardClient(localtestroot, "clientA1"));
    ASSERT_TRUE(pclientA1->login_fetchnodes(session));
    ASSERT_EQ(pclientA1->basefolderhandle, clientA2.basefolderhandle);
    waitonsyncs(DEFAULTWAIT, pclientA1.get(), &clientA2);

    out() << "*********************  check everything matches (model has expected state of remote and local)" << endl;
    ASSERT_TRUE(pclientA1->confirmModel_mainthread(model1.findnode("f"), backupId1));
    model2.ensureLocalDebrisTmpLock("f"); // since we downloaded files
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model2.findnode("f"), backupId2));
}

GTEST_TEST(Sync, BasicSync_ResumeSyncFromSessionAfterClashingLocalAddRemoteDelete)
{
    fs::path localtestroot = makeNewTestRoot();
    unique_ptr<StandardClient> pclientA1(new StandardClient(localtestroot, "clientA1"));   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(pclientA1->login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(pclientA1->basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 3, 3, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = pclientA1->setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);
    waitonsyncs(std::chrono::seconds(4), pclientA1.get(), &clientA2);
    pclientA1->logcb = clientA2.logcb = true;

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(pclientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    // save session A1
    string session;
    pclientA1->client.dumpsession(session);
    fs::path sync1path = pclientA1->syncSet(backupId1).localpath;

    // logout A1 (but keep caches on disk)
    pclientA1->localLogout();

    // remove remote folder via A2
    future<bool> p1 = clientA2.thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb) { sc.deleteremote("f/f_1", pb); });
    ASSERT_TRUE(waitonresults(&p1));

    // add local folders in A1 on disk folder
    ASSERT_TRUE(buildLocalFolders(sync1path / "f_1/f_1_2", "newlocal", 2, 2, 2));

    // get sync2 activity out of the way
    waitonsyncs(std::chrono::seconds(4), &clientA2);

    // resume A1 session (with sync), see if A2 nodes and localnodes get in sync again
    pclientA1.reset(new StandardClient(localtestroot, "clientA1"));
    ASSERT_TRUE(pclientA1->login_fetchnodes(session));
    ASSERT_EQ(pclientA1->basefolderhandle, clientA2.basefolderhandle);
    waitonsyncs(std::chrono::seconds(10), pclientA1.get(), &clientA2);

    // check everything matches (model has expected state of remote and local)
    model.findnode("f/f_1/f_1_2")->addkid(model.buildModelSubdirs("newlocal", 2, 2, 2));
    ASSERT_TRUE(model.movetosynctrash("f/f_1", "f"));
    ASSERT_TRUE(pclientA1->confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(model.removesynctrash("f", "f_1/f_1_2/newlocal"));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
}


GTEST_TEST(Sync, CmdChecks_RRAttributeAfterMoveNode)
{
    fs::path localtestroot = makeNewTestRoot();
    unique_ptr<StandardClient> pclientA1(new StandardClient(localtestroot, "clientA1"));   // user 1 client 1

    ASSERT_TRUE(pclientA1->login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 3, 3));

    Node* f = pclientA1->drillchildnodebyname(pclientA1->gettestbasenode(), "f");
    handle original_f_handle = f->nodehandle;
    handle original_f_parent_handle = f->parent->nodehandle;

    // make sure there are no 'f' in the rubbish
    auto fv = pclientA1->drillchildnodesbyname(pclientA1->getcloudrubbishnode(), "f");
    future<bool> fb = pclientA1->thread_do<bool>([&fv](StandardClient& sc, PromiseBoolSP pb) { sc.deleteremotenodes(fv, pb); });
    ASSERT_TRUE(waitonresults(&fb));

    f = pclientA1->drillchildnodebyname(pclientA1->getcloudrubbishnode(), "f");
    ASSERT_TRUE(f == nullptr);


    // remove remote folder via A2
    future<bool> p1 = pclientA1->thread_do<bool>([](StandardClient& sc, PromiseBoolSP pb)
        {
            sc.movenodetotrash("f", pb);
        });
    ASSERT_TRUE(waitonresults(&p1));

    WaitMillisec(3000);  // allow for attribute delivery too

    f = pclientA1->drillchildnodebyname(pclientA1->getcloudrubbishnode(), "f");
    ASSERT_TRUE(f != nullptr);

    // check the restore-from-trash handle got set, and correctly
    nameid rrname = AttrMap::string2nameid("rr");
    ASSERT_EQ(f->nodehandle, original_f_handle);
    ASSERT_EQ(f->attrs.map[rrname], string(Base64Str<MegaClient::NODEHANDLE>(original_f_parent_handle)));
    ASSERT_EQ(f->attrs.map[rrname], string(Base64Str<MegaClient::NODEHANDLE>(pclientA1->gettestbasenode()->nodehandle)));

    // move it back

    p1 = pclientA1->thread_do<bool>([&](StandardClient& sc, PromiseBoolSP pb)
    {
        sc.movenode(f->nodehandle, pclientA1->basefolderhandle, pb);
    });
    ASSERT_TRUE(waitonresults(&p1));

    WaitMillisec(3000);  // allow for attribute delivery too

    // check it's back and the rr attribute is gone
    f = pclientA1->drillchildnodebyname(pclientA1->gettestbasenode(), "f");
    ASSERT_TRUE(f != nullptr);
    ASSERT_EQ(f->attrs.map[rrname], string());
}


#ifdef __linux__
GTEST_TEST(Sync, BasicSync_SpecialCreateFile)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 2, 2));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 2, 2, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);

    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;
    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    // make new folders (and files) in the local filesystem and see if we catch up in A1 and A2 (adder and observer syncs)
    ASSERT_TRUE(createSpecialFiles(clientA1.syncSet(backupId1).localpath / "f_0", "newkid", 2));

    for (int i = 0; i < 2; ++i)
    {
        string filename = "file" + to_string(i) + "_" + "newkid";
        model.findnode("f/f_0")->addkid(model.makeModelSubfile(filename));
    }

    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    model.ensureLocalDebrisTmpLock("f"); // since we downloaded files
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
}
#endif

GTEST_TEST(Sync, DISABLED_BasicSync_moveAndDeleteLocalFile)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 1, 1));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 1, 1, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);

    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;
    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));


    // move something in the local filesystem and see if we catch up in A1 and A2 (deleter and observer syncs)
    error_code rename_error;
    fs::rename(clientA1.syncSet(backupId1).localpath / "f_0", clientA1.syncSet(backupId1).localpath / "renamed", rename_error);
    ASSERT_TRUE(!rename_error) << rename_error;
    fs::remove(clientA1.syncSet(backupId1).localpath / "renamed");

    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(model.movetosynctrash("f/f_0", "f"));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
    ASSERT_TRUE(model.removesynctrash("f"));
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
}

namespace {

string makefa(const string& name, int fakecrc, int mtime)
{
    AttrMap attrs;
    attrs.map['n'] = name;

    FileFingerprint ff;
    ff.crc[0] = ff.crc[1] = ff.crc[2] = ff.crc[3] = fakecrc;
    ff.mtime = mtime;
    ff.serializefingerprint(&attrs.map['c']);

    string attrjson;
    attrs.getjson(&attrjson);
    return attrjson;
}

Node* makenode(MegaClient& mc, handle parent, ::mega::nodetype_t type, m_off_t size, handle owner, const string& attrs, ::mega::byte* key)
{
    static handle handlegenerator = 10;
    std::vector<Node*> dp;
    auto newnode = new Node(&mc, &dp, ++handlegenerator, parent, type, size, owner, nullptr, 1);

    newnode->setkey(key);
    newnode->attrstring.reset(new string);

    SymmCipher sc;
    sc.setkey(key, type);
    mc.makeattr(&sc, newnode->attrstring, attrs.c_str());

    int attrlen = int(newnode->attrstring->size());
    string base64attrstring;
    base64attrstring.resize(static_cast<size_t>(attrlen * 4 / 3 + 4));
    base64attrstring.resize(static_cast<size_t>(Base64::btoa((::mega::byte *)newnode->attrstring->data(), int(newnode->attrstring->size()), (char *)base64attrstring.data())));

    *newnode->attrstring = base64attrstring;

    return newnode;
}

} // anonymous

GTEST_TEST(Sync, NodeSorting_forPhotosAndVideos)
{
    fs::path localtestroot = makeNewTestRoot();
    StandardClient standardclient(localtestroot, "sortOrderTests");
    auto& client = standardclient.client;

    handle owner = 99999;

    ::mega::byte key[] = { 0x01, 0x02, 0x03, 0x04, 0x01, 0x02, 0x03, 0x04, 0x01, 0x02, 0x03, 0x04, 0x01, 0x02, 0x03, 0x04, 0x01, 0x02, 0x03, 0x04, 0x01, 0x02, 0x03, 0x04, 0x01, 0x02, 0x03, 0x04, 0x01, 0x02, 0x03, 0x04 };

    // first 3 are root nodes:
    auto cloudroot = makenode(client, UNDEF, ROOTNODE, -1, owner, makefa("root", 1, 1), key);
    makenode(client, UNDEF, INCOMINGNODE, -1, owner, makefa("inbox", 1, 1), key);
    makenode(client, UNDEF, RUBBISHNODE, -1, owner, makefa("bin", 1, 1), key);

    // now some files to sort
    auto photo1 = makenode(client, cloudroot->nodehandle, FILENODE, 9999, owner, makefa("abc.jpg", 1, 1570673890), key);
    auto photo2 = makenode(client, cloudroot->nodehandle, FILENODE, 9999, owner, makefa("cba.png", 1, 1570673891), key);
    auto video1 = makenode(client, cloudroot->nodehandle, FILENODE, 9999, owner, makefa("xyz.mov", 1, 1570673892), key);
    auto video2 = makenode(client, cloudroot->nodehandle, FILENODE, 9999, owner, makefa("zyx.mp4", 1, 1570673893), key);
    auto otherfile = makenode(client, cloudroot->nodehandle, FILENODE, 9999, owner, makefa("ASDF.fsda", 1, 1570673894), key);
    auto otherfolder = makenode(client, cloudroot->nodehandle, FOLDERNODE, -1, owner, makefa("myfolder", 1, 1570673895), key);

    node_vector v{ photo1, photo2, video1, video2, otherfolder, otherfile };
    for (auto n : v) n->setkey(key);

    MegaApiImpl::sortByComparatorFunction(v, MegaApi::ORDER_PHOTO_ASC, client);
    node_vector v2{ photo1, photo2, video1, video2, otherfolder, otherfile };
    ASSERT_EQ(v, v2);

    MegaApiImpl::sortByComparatorFunction(v, MegaApi::ORDER_PHOTO_DESC, client);
    node_vector v3{ photo2, photo1, video2, video1, otherfolder, otherfile };
    ASSERT_EQ(v, v3);

    MegaApiImpl::sortByComparatorFunction(v, MegaApi::ORDER_VIDEO_ASC, client);
    node_vector v4{ video1, video2, photo1, photo2, otherfolder, otherfile };
    ASSERT_EQ(v, v4);

    MegaApiImpl::sortByComparatorFunction(v, MegaApi::ORDER_VIDEO_DESC, client);
    node_vector v5{ video2, video1, photo2, photo1, otherfolder, otherfile };
    ASSERT_EQ(v, v5);
}


GTEST_TEST(Sync, PutnodesForMultipleFolders)
{
    fs::path localtestroot = makeNewTestRoot();
    StandardClient standardclient(localtestroot, "PutnodesForMultipleFolders");
    ASSERT_TRUE(standardclient.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD", true));

    vector<NewNode> newnodes(4);

    standardclient.client.putnodes_prepareOneFolder(&newnodes[0], "folder1");
    standardclient.client.putnodes_prepareOneFolder(&newnodes[1], "folder2");
    standardclient.client.putnodes_prepareOneFolder(&newnodes[2], "folder2.1");
    standardclient.client.putnodes_prepareOneFolder(&newnodes[3], "folder2.2");

    newnodes[1].nodehandle = newnodes[2].parenthandle = newnodes[3].parenthandle = 2;

    handle targethandle = standardclient.client.rootnodes[0];

    std::atomic<bool> putnodesDone{false};
    standardclient.resultproc.prepresult(StandardClient::PUTNODES,  ++next_request_tag,
        [&](){ standardclient.client.putnodes(targethandle, move(newnodes), nullptr); },
        [&putnodesDone](error e) { putnodesDone = true; return true; });

    while (!putnodesDone)
    {
        WaitMillisec(100);
    }

    Node* cloudRoot = standardclient.client.nodebyhandle(targethandle);

    ASSERT_TRUE(nullptr != standardclient.drillchildnodebyname(cloudRoot, "folder1"));
    ASSERT_TRUE(nullptr != standardclient.drillchildnodebyname(cloudRoot, "folder2"));
    ASSERT_TRUE(nullptr != standardclient.drillchildnodebyname(cloudRoot, "folder2/folder2.1"));
    ASSERT_TRUE(nullptr != standardclient.drillchildnodebyname(cloudRoot, "folder2/folder2.2"));
}


#ifndef _WIN32_SUPPORTS_SYMLINKS_IT_JUST_NEEDS_TURNING_ON
GTEST_TEST(Sync, BasicSync_CreateAndDeleteLink)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 1, 1));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 1, 1, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);

    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;
    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));


    // move something in the local filesystem and see if we catch up in A1 and A2 (deleter and observer syncs)
    error_code linkage_error;
    fs::create_symlink(clientA1.syncSet(backupId1).localpath / "f_0", clientA1.syncSet(backupId1).localpath / "linked", linkage_error);
    ASSERT_TRUE(!linkage_error) << linkage_error;

    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    //check client 2 is unaffected
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));


    fs::remove(clientA1.syncSet(backupId1).localpath / "linked");
    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    //check client 2 is unaffected
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
}

GTEST_TEST(Sync, BasicSync_CreateRenameAndDeleteLink)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 1, 1));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 1, 1, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);

    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;
    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));


    // move something in the local filesystem and see if we catch up in A1 and A2 (deleter and observer syncs)
    error_code linkage_error;
    fs::create_symlink(clientA1.syncSet(backupId1).localpath / "f_0", clientA1.syncSet(backupId1).localpath / "linked", linkage_error);
    ASSERT_TRUE(!linkage_error) << linkage_error;

    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    //check client 2 is unaffected
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    fs::rename(clientA1.syncSet(backupId1).localpath / "linked", clientA1.syncSet(backupId1).localpath / "linkrenamed", linkage_error);

    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    //check client 2 is unaffected
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    fs::remove(clientA1.syncSet(backupId1).localpath / "linkrenamed");

    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    //check client 2 is unaffected
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
}


#ifndef WIN32

// what is supposed to happen for this one?  It seems that the `linked` symlink is no longer ignored on windows?  client2 is affected!

GTEST_TEST(Sync, BasicSync_CreateAndReplaceLinkLocally)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 1, 1));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 1, 1, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);

    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;
    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));


    // move something in the local filesystem and see if we catch up in A1 and A2 (deleter and observer syncs)
    error_code linkage_error;
    fs::create_symlink(clientA1.syncSet(backupId1).localpath / "f_0", clientA1.syncSet(backupId1).localpath / "linked", linkage_error);
    ASSERT_TRUE(!linkage_error) << linkage_error;

    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    //check client 2 is unaffected
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
    fs::rename(clientA1.syncSet(backupId1).localpath / "f_0", clientA1.syncSet(backupId1).localpath / "linked", linkage_error);

    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    //check client 2 is unaffected
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    fs::remove(clientA1.syncSet(backupId1).localpath / "linked");
    ASSERT_TRUE(createNameFile(clientA1.syncSet(backupId1).localpath, "linked"));

    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    model.findnode("f")->addkid(model.makeModelSubfile("linked"));
    model.ensureLocalDebrisTmpLock("f"); // since we downloaded files

    //check client 2 is as expected
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));
}


GTEST_TEST(Sync, BasicSync_CreateAndReplaceLinkUponSyncDown)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();
    StandardClient clientA1(localtestroot, "clientA1");   // user 1 client 1
    StandardClient clientA2(localtestroot, "clientA2");   // user 1 client 2

    ASSERT_TRUE(clientA1.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "f", 1, 1));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD"));
    ASSERT_EQ(clientA1.basefolderhandle, clientA2.basefolderhandle);

    Model model;
    model.root->addkid(model.buildModelSubdirs("f", 1, 1, 0));

    // set up sync for A1, it should build matching local folders
    handle backupId1 = clientA1.setupSync_mainthread("sync1", "f");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA2.setupSync_mainthread("sync2", "f");
    ASSERT_NE(backupId2, UNDEF);

    waitonsyncs(std::chrono::seconds(4), &clientA1, &clientA2);
    clientA1.logcb = clientA2.logcb = true;
    // check everything matches (model has expected state of remote and local)
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    // move something in the local filesystem and see if we catch up in A1 and A2 (deleter and observer syncs)
    error_code linkage_error;
    fs::create_symlink(clientA1.syncSet(backupId1).localpath / "f_0", clientA1.syncSet(backupId1).localpath / "linked", linkage_error);
    ASSERT_TRUE(!linkage_error) << linkage_error;

    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    //check client 2 is unaffected
    ASSERT_TRUE(clientA2.confirmModel_mainthread(model.findnode("f"), backupId2));

    ASSERT_TRUE(createNameFile(clientA2.syncSet(backupId2).localpath, "linked"));

    // let them catch up
    waitonsyncs(DEFAULTWAIT, &clientA1, &clientA2);

    model.findnode("f")->addkid(model.makeModelSubfolder("linked")); //notice: the deleted here is folder because what's actually deleted is a symlink that points to a folder
                                                                     //ideally we could add full support for symlinks in this tests suite

    model.movetosynctrash("f/linked","f");
    model.findnode("f")->addkid(model.makeModelSubfile("linked"));
    model.ensureLocalDebrisTmpLock("f"); // since we downloaded files

    //check client 2 is as expected
    ASSERT_TRUE(clientA1.confirmModel_mainthread(model.findnode("f"), backupId1));
}
#endif

#endif

struct TwoWaySyncSymmetryCase
{
    enum Action { action_rename, action_moveWithinSync, action_moveOutOfSync, action_moveIntoSync, action_delete, action_numactions };

    enum MatchState { match_exact,      // the sync destination has the exact same file/folder at the same relative path
                      match_older,      // the sync destination has an older file/folder at the same relative path
                      match_newer,      // the sync destination has a newer file/folder at the same relative path
                      match_absent };   // the sync destination has no node at the same relative path

    Action action = action_rename;
    bool selfChange = false; // changed by our own client or another
    bool up = false;  // or down - sync direction
    bool file = false;  // or folder.  Which one this test changes
    bool pauseDuringAction = false;
    Model localModel;
    Model remoteModel;
    handle backupId = UNDEF;

    bool printTreesBeforeAndAfter = false;

    struct State
    {
        StandardClient& steadyClient;
        StandardClient& resumeClient;
        StandardClient& nonsyncClient;
        fs::path localBaseFolderSteady;
        fs::path localBaseFolderResume;
        std::string remoteBaseFolder = "twoway";   // leave out initial / so we can drill down from root node
        std::string first_test_name;
        fs::path first_test_initiallocalfolders;

        State(StandardClient& ssc, StandardClient& rsc, StandardClient& sc2) : steadyClient(ssc), resumeClient(rsc), nonsyncClient(sc2) {}
    };

    State& state;
    TwoWaySyncSymmetryCase(State& wholestate) : state(wholestate) {}

    // todo: remote changes made by client (of this sync) or other client

    std::string actionName()
    {
        switch (action)
        {
        case action_rename: return "rename";
        case action_moveWithinSync: return "move";
        case action_moveOutOfSync: return "moveOut";
        case action_moveIntoSync: return "moveIn";
        case action_delete: return "delete";
        default: assert(false); return "";
        }
    }

    std::string matchName(MatchState m)
    {
        switch (m)
        {
            case match_exact: return "exact";
            case match_older: return "older";
            case match_newer: return "newer";
            case match_absent: return "absent";
        }
        return "bad enum";
    }

    std::string name()
    {
        return  actionName() +
                (up?"_up" : "_down") +
                (selfChange?"_self":"_other") +
                (file?"_file":"_folder") +
                (pauseDuringAction?"_resumed":"");
    }

    fs::path localTestBasePathSteady;
    fs::path localTestBasePathResume;
    std::string remoteTestBasePath;

    Model& sourceModel() { return up ? localModel : remoteModel; }
    Model& destinationModel() { return up ? remoteModel : localModel; }

    StandardClient& client1() { return pauseDuringAction ? state.resumeClient : state.steadyClient; }
    StandardClient& changeClient() { return selfChange ? client1() : state.nonsyncClient; }

    fs::path localTestBasePath() { return pauseDuringAction ? localTestBasePathResume : localTestBasePathSteady; }

    void makeMtimeFile(std::string name, int mtime_delta, Model& m1, Model& m2)
    {
        createNameFile(state.first_test_initiallocalfolders, name);
        auto initial_mtime = fs::last_write_time(state.first_test_initiallocalfolders / name);
        fs::last_write_time(state.first_test_initiallocalfolders / name, initial_mtime + std::chrono::seconds(mtime_delta));
        fs::rename(state.first_test_initiallocalfolders / name, state.first_test_initiallocalfolders / "f" / name); // move it after setting the time to be 100% sure the sync sees it with the adjusted mtime only
        m1.findnode("f")->addkid(m1.makeModelSubfile(name));
        m2.findnode("f")->addkid(m2.makeModelSubfile(name));
    }

    PromiseBoolSP cloudCopySetupPromise = newPromiseBoolSP();

    // prepares a local folder for testing, which will be two-way synced before the test
    void SetupForSync()
    {
        localTestBasePathSteady = state.localBaseFolderSteady / name();
        localTestBasePathResume = state.localBaseFolderResume / name();
        remoteTestBasePath = state.remoteBaseFolder + "/" + name();
        std::error_code ec;

        fs::create_directories(localTestBasePathSteady, ec);
        ASSERT_TRUE(!ec);
        fs::create_directories(localTestBasePathResume, ec);
        ASSERT_TRUE(!ec);

        localModel.root->addkid(localModel.buildModelSubdirs("f", 2, 2, 2));
        localModel.root->addkid(localModel.buildModelSubdirs("outside", 2, 1, 1));
        remoteModel.root->addkid(remoteModel.buildModelSubdirs("f", 2, 2, 2));
        remoteModel.root->addkid(remoteModel.buildModelSubdirs("outside", 2, 1, 1));

        // for the first one, copy that to the cloud.
        // for subsequent, duplicate in the cloud with this test's name.

        Node* testRoot = changeClient().client.nodebyhandle(changeClient().basefolderhandle);
        Node* n2 = changeClient().drillchildnodebyname(testRoot, state.remoteBaseFolder);
        if (state.first_test_name.empty())
        {
            state.first_test_name = name();
            state.first_test_initiallocalfolders = pauseDuringAction ? localTestBasePathResume : localTestBasePathSteady;

            ASSERT_TRUE(buildLocalFolders(state.first_test_initiallocalfolders, "f", 2, 2, 2));
            ASSERT_TRUE(buildLocalFolders(state.first_test_initiallocalfolders, "outside", 2, 1, 1));
            makeMtimeFile("file_older_1", -3600, localModel, remoteModel);
            makeMtimeFile("file_newer_1", 3600, localModel, remoteModel);
            makeMtimeFile("file_older_2", -3600, localModel, remoteModel);
            makeMtimeFile("file_newer_2", 3600, localModel, remoteModel);

            auto pb = newPromiseBoolSP();
            changeClient().uploadFolderTree(state.first_test_initiallocalfolders, n2, pb);
            ASSERT_TRUE(pb->get_future().get());

            auto pb2 = newPromiseBoolSP();
            std::atomic<int> inprogress(0);
            changeClient().uploadFilesInTree(state.first_test_initiallocalfolders, n2, inprogress, pb2);
            ASSERT_TRUE(pb2->get_future().get());
            out() << "Uploaded tree for " << name() << endl;
        }
        else
        {
            fs::copy(state.first_test_initiallocalfolders,
                    pauseDuringAction ? localTestBasePathResume : localTestBasePathSteady,
                    fs::copy_options::recursive,  ec);
            ASSERT_TRUE(!ec);

            Node* n1 = changeClient().drillchildnodebyname(testRoot, state.remoteBaseFolder + "/" + state.first_test_name);
            changeClient().cloudCopyTreeAs(n1, n2, name(), cloudCopySetupPromise);
            ASSERT_TRUE(cloudCopySetupPromise->get_future().get());
            out() << "Copied cloud tree for " << name() << endl;

            localModel.findnode("f")->addkid(localModel.makeModelSubfile("file_older_1"));
            remoteModel.findnode("f")->addkid(remoteModel.makeModelSubfile("file_older_1"));
            localModel.findnode("f")->addkid(localModel.makeModelSubfile("file_newer_1"));
            remoteModel.findnode("f")->addkid(remoteModel.makeModelSubfile("file_newer_1"));
            localModel.findnode("f")->addkid(localModel.makeModelSubfile("file_older_2"));
            remoteModel.findnode("f")->addkid(remoteModel.makeModelSubfile("file_older_2"));
            localModel.findnode("f")->addkid(localModel.makeModelSubfile("file_newer_2"));
            remoteModel.findnode("f")->addkid(remoteModel.makeModelSubfile("file_newer_2"));
        }

    }

    void SetupTwoWaySync()
    {
        string localname, syncrootpath((localTestBasePath() / "f").u8string());
        client1().client.fsaccess->path2local(&syncrootpath, &localname);

        Node* testRoot = client1().client.nodebyhandle(client1().basefolderhandle);
        Node* n = client1().drillchildnodebyname(testRoot, remoteTestBasePath + "/f");
        ASSERT_TRUE(!!n);

        //SyncConfig config(syncrootpath, n->nodehandle, 0, {}, (SyncConfig::TYPE_TWOWAY), false, false);

        auto lsfr = syncrootpath.erase(0, client1().fsBasePath.u8string().size()+1);
        auto rsfr = remoteTestBasePath + "/f";

        backupId = client1().setupSync_mainthread(lsfr, rsfr);
        ASSERT_NE(backupId, UNDEF);
    }

    //void PauseTwoWaySync()
    //{
    //    client1().delSync_mainthread(sync_tag, true);
    //}

    //void ResumeTwoWaySync()
    //{
    //    SetupTwoWaySync();
    //}

    void remote_rename(std::string nodepath, std::string newname, bool updatemodel, bool reportaction, bool deleteTargetFirst)
    {
        if (deleteTargetFirst) remote_delete(parentpath(nodepath) + "/" + newname, updatemodel, reportaction, true); // in case the target already exists

        if (updatemodel) remoteModel.emulate_rename(nodepath, newname);

        Node* testRoot = changeClient().client.nodebyhandle(client1().basefolderhandle);
        Node* n = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + nodepath);
        ASSERT_TRUE(!!n);

        if (reportaction) out() << name() << " action: remote rename " << n->displaypath() << " to " << newname << endl;

        n->attrs.map['n'] = newname;
        auto e = changeClient().client.setattr(n, nullptr);

        ASSERT_EQ(API_OK, e);
    }

    void remote_move(std::string nodepath, std::string newparentpath, bool updatemodel, bool reportaction, bool deleteTargetFirst)
    {

        if (deleteTargetFirst) remote_delete(newparentpath + "/" + leafname(nodepath), updatemodel, reportaction, true); // in case the target already exists

        if (updatemodel) remoteModel.emulate_move(nodepath, newparentpath);

        Node* testRoot = changeClient().client.nodebyhandle(changeClient().basefolderhandle);
        Node* n1 = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + nodepath);
        Node* n2 = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + newparentpath);
        ASSERT_TRUE(!!n1);
        ASSERT_TRUE(!!n2);

        if (reportaction) out() << name() << " action: remote move " << n1->displaypath() << " to " << n2->displaypath() << endl;

        auto e = changeClient().client.rename(n1, n2, SYNCDEL_NONE, UNDEF, nullptr);
        ASSERT_EQ(API_OK, e);
    }

    void remote_copy(std::string nodepath, std::string newparentpath, bool updatemodel, bool reportaction)
    {
        if (updatemodel) remoteModel.emulate_copy(nodepath, newparentpath);

        Node* testRoot = changeClient().client.nodebyhandle(changeClient().basefolderhandle);
        Node* n1 = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + nodepath);
        Node* n2 = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + newparentpath);
        ASSERT_TRUE(!!n1);
        ASSERT_TRUE(!!n2);

        if (reportaction) out() << name() << " action: remote copy " << n1->displaypath() << " to " << n2->displaypath() << endl;

        TreeProcCopy tc;
        changeClient().client.proctree(n1, &tc, false, true);
        tc.allocnodes();
        changeClient().client.proctree(n1, &tc, false, true);
        tc.nn[0].parenthandle = UNDEF;

        SymmCipher key;
        AttrMap attrs;
        string attrstring;
        key.setkey((const ::mega::byte*)tc.nn[0].nodekey.data(), n1->type);
        attrs = n1->attrs;
        attrs.getjson(&attrstring);
        client1().client.makeattr(&key, tc.nn[0].attrstring, attrstring.c_str());
        changeClient().client.putnodes(n2->nodehandle, move(tc.nn));
    }

    void remote_renamed_copy(std::string nodepath, std::string newparentpath, string newname, bool updatemodel, bool reportaction)
    {
        if (updatemodel)
        {
            remoteModel.emulate_rename_copy(nodepath, newparentpath, newname);
        }

        Node* testRoot = changeClient().client.nodebyhandle(changeClient().basefolderhandle);
        Node* n1 = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + nodepath);
        Node* n2 = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + newparentpath);
        ASSERT_TRUE(!!n1);
        ASSERT_TRUE(!!n2);

        if (reportaction) out() << name() << " action: remote rename + copy " << n1->displaypath() << " to " << n2->displaypath() << " as " << newname << endl;

        TreeProcCopy tc;
        changeClient().client.proctree(n1, &tc, false, true);
        tc.allocnodes();
        changeClient().client.proctree(n1, &tc, false, true);
        tc.nn[0].parenthandle = UNDEF;

        SymmCipher key;
        AttrMap attrs;
        string attrstring;
        key.setkey((const ::mega::byte*)tc.nn[0].nodekey.data(), n1->type);
        attrs = n1->attrs;
        client1().client.fsaccess->normalize(&newname);
        attrs.map['n'] = newname;
        attrs.getjson(&attrstring);
        client1().client.makeattr(&key, tc.nn[0].attrstring, attrstring.c_str());
        changeClient().client.putnodes(n2->nodehandle, move(tc.nn));
    }

    void remote_renamed_move(std::string nodepath, std::string newparentpath, string newname, bool updatemodel, bool reportaction)
    {
        if (updatemodel)
        {
            remoteModel.emulate_rename_copy(nodepath, newparentpath, newname);
        }

        Node* testRoot = changeClient().client.nodebyhandle(changeClient().basefolderhandle);
        Node* n1 = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + nodepath);
        Node* n2 = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + newparentpath);
        ASSERT_TRUE(!!n1);
        ASSERT_TRUE(!!n2);

        if (reportaction) out() << name() << " action: remote rename + move " << n1->displaypath() << " to " << n2->displaypath() << " as " << newname << endl;

        error e = changeClient().client.rename(n1, n2, SYNCDEL_NONE, UNDEF, newname.c_str());
        EXPECT_EQ(e, API_OK);
    }

    void remote_delete(std::string nodepath, bool updatemodel, bool reportaction, bool mightNotExist)
    {

        Node* testRoot = changeClient().client.nodebyhandle(changeClient().basefolderhandle);
        Node* n = changeClient().drillchildnodebyname(testRoot, remoteTestBasePath + "/" + nodepath);
        if (mightNotExist && !n) return;  // eg when checking to remove an item that is a move target but there isn't one

        ASSERT_TRUE(!!n);

        if (reportaction) out() << name() << " action: remote delete " << n->displaypath() << endl;

        if (updatemodel) remoteModel.emulate_delete(nodepath);

        auto e = changeClient().client.unlink(n, false, ++next_request_tag);
        ASSERT_TRUE(!e);
    }

    fs::path fixSeparators(std::string p)
    {
        for (auto& c : p)
            if (c == '/')
                c = fs::path::preferred_separator;
        return fs::u8path(p);
    }

    void local_rename(std::string path, std::string newname, bool updatemodel, bool reportaction, bool deleteTargetFirst)
    {
        if (deleteTargetFirst) local_delete(parentpath(path) + "/" + newname, updatemodel, reportaction, true); // in case the target already exists

        if (updatemodel) localModel.emulate_rename(path, newname);

        fs::path p1(localTestBasePath());
        p1 /= fixSeparators(path);
        fs::path p2 = p1.parent_path() / newname;

        if (reportaction) out() << name() << " action: local rename " << p1 << " to " << p2 << endl;

        std::error_code ec;
        for (int i = 0; i < 5; ++i)
        {
            fs::rename(p1, p2, ec);
            if (!ec) break;
            WaitMillisec(100);
        }
        ASSERT_TRUE(!ec) << "local_rename " << p1 << " to " << p2 << " failed: " << ec.message();
    }

    void local_move(std::string from, std::string to, bool updatemodel, bool reportaction, bool deleteTargetFirst)
    {
        if (deleteTargetFirst) local_delete(to + "/" + leafname(from), updatemodel, reportaction, true);

        if (updatemodel) localModel.emulate_move(from, to);

        fs::path p1(localTestBasePath());
        fs::path p2(localTestBasePath());
        p1 /= fixSeparators(from);
        p2 /= fixSeparators(to);
        p2 /= p1.filename();  // non-existing file in existing directory case

        if (reportaction) out() << name() << " action: local move " << p1 << " to " << p2 << endl;

        std::error_code ec;
        fs::rename(p1, p2, ec);
        if (ec)
        {
            fs::remove_all(p2, ec);
            fs::rename(p1, p2, ec);
        }
        ASSERT_TRUE(!ec) << "local_move " << p1 << " to " << p2 << " failed: " << ec.message();
    }

    void local_copy(std::string from, std::string to, bool updatemodel, bool reportaction)
    {
        if (updatemodel) localModel.emulate_copy(from, to);

        fs::path p1(localTestBasePath());
        fs::path p2(localTestBasePath());
        p1 /= fixSeparators(from);
        p2 /= fixSeparators(to);

        if (reportaction) out() << name() << " action: local copy " << p1 << " to " << p2 << endl;

        std::error_code ec;
        fs::copy(p1, p2, ec);
        ASSERT_TRUE(!ec) << "local_copy " << p1 << " to " << p2 << " failed: " << ec.message();
    }

    void local_delete(std::string path, bool updatemodel, bool reportaction, bool mightNotExist)
    {
        fs::path p(localTestBasePath());
        p /= fixSeparators(path);

        if (mightNotExist && !fs::exists(p)) return;

        if (reportaction) out() << name() << " action: local_delete " << p << endl;

        std::error_code ec;
        fs::remove_all(p, ec);
        ASSERT_TRUE(!ec) << "local_delete " << p << " failed: " << ec.message();
        if (updatemodel) localModel.emulate_delete(path);
    }

    void source_rename(std::string nodepath, std::string newname, bool updatemodel, bool reportaction, bool deleteTargetFirst)
    {
        if (up) local_rename(nodepath, newname, updatemodel, reportaction, deleteTargetFirst);
        else remote_rename(nodepath, newname, updatemodel, reportaction, deleteTargetFirst);
    }

    void source_move(std::string nodepath, std::string newparentpath, bool updatemodel, bool reportaction, bool deleteTargetFirst)
    {
        if (up) local_move(nodepath, newparentpath, updatemodel, reportaction, deleteTargetFirst);
        else remote_move(nodepath, newparentpath, updatemodel, reportaction, deleteTargetFirst);
    }

    void source_copy(std::string nodepath, std::string newparentpath, bool updatemodel, bool reportaction)
    {
        if (up) local_copy(nodepath, newparentpath, updatemodel, reportaction);
        else remote_copy(nodepath, newparentpath, updatemodel, reportaction);
    }

    void source_delete(std::string nodepath, bool updatemodel, bool reportaction = false)
    {
        if (up) local_delete(nodepath, updatemodel, reportaction, false);
        else remote_delete(nodepath, updatemodel, reportaction, false);
    }

    void destination_rename(std::string nodepath, std::string newname, bool updatemodel, bool reportaction, bool deleteTargetFirst)
    {
        if (!up) local_rename(nodepath, newname, updatemodel, reportaction, deleteTargetFirst);
        else remote_rename(nodepath, newname, updatemodel, reportaction, deleteTargetFirst);
    }

    void destination_move(std::string nodepath, std::string newparentpath, bool updatemodel, bool reportaction, bool deleteTargetFirst)
    {
        if (!up) local_move(nodepath, newparentpath, updatemodel, reportaction, deleteTargetFirst);
        else remote_move(nodepath, newparentpath, updatemodel, reportaction, deleteTargetFirst);
    }

    void destination_copy(std::string nodepath, std::string newparentpath, bool updatemodel, bool reportaction)
    {
        if (!up) local_copy(nodepath, newparentpath, updatemodel, reportaction);
        else remote_copy(nodepath, newparentpath, updatemodel, reportaction);
    }

    void destination_delete(std::string nodepath, bool updatemodel, bool reportaction)
    {
        if (!up) local_delete(nodepath, updatemodel, reportaction, false);
        else remote_delete(nodepath, updatemodel, reportaction, false);
    }

    void destination_copy_renamed(std::string sourcefolder, std::string oldname, std::string newname, std::string targetfolder, bool updatemodel, bool reportaction, bool deleteTargetFirst)
    {
        if (up)
        {
            remote_renamed_copy(sourcefolder + "/" + oldname, targetfolder, newname, updatemodel, reportaction);
            return;
        }

        // avoid name clashes in any one folder
        if (sourcefolder != "f") destination_copy(sourcefolder + "/" + oldname, "f", updatemodel, reportaction);
        destination_rename("f/" + oldname, newname, updatemodel, reportaction, false);
        if (targetfolder != "f") destination_move("f/" + newname, targetfolder, updatemodel, reportaction, deleteTargetFirst);
    }

    void destination_rename_move(std::string sourcefolder, std::string oldname, std::string newname, std::string targetfolder, bool updatemodel, bool reportaction, bool deleteTargetFirst, std::string deleteNameInTargetFirst)
    {
        if (up)
        {
            remote_renamed_move(sourcefolder + "/" + oldname, targetfolder, newname, updatemodel, reportaction);
            return;
        }

        if (!deleteNameInTargetFirst.empty()) destination_delete(targetfolder + "/" + deleteNameInTargetFirst, updatemodel, reportaction);
        destination_rename("f/" + oldname, newname, updatemodel, reportaction, false);
        destination_move("f/" + newname, targetfolder, updatemodel, reportaction, deleteTargetFirst);
    }

    void fileMayDiffer(std::string filepath)
    {
        fs::path p(localTestBasePath());
        p /= fixSeparators(filepath);

        client1().localFSFilesThatMayDiffer.insert(p);
        out() << "File may differ: " << p << endl;
    }

    // Two-way sync has been started and is stable.  Now perform the test action

    enum ModifyStage { Prepare, MainAction };

    void PrintLocalTree(fs::path p)
    {
        out() << p << endl;
        if (fs::is_directory(p))
        {
            for (auto i = fs::directory_iterator(p); i != fs::directory_iterator(); ++i)
            {
                PrintLocalTree(*i);
            }
        }
    }

    void PrintRemoteTree(Node* n, string prefix = "")
    {
        prefix += string("/") + n->displayname();
        out() << prefix << endl;
        if (n->type == FILENODE) return;
        for (auto& c : n->children)
        {
            PrintRemoteTree(c, prefix);
        }
    }

    void PrintModelTree(Model::ModelNode* n, string prefix = "")
    {
        prefix += string("/") + n->name;
        out() << prefix << endl;
        if (n->type == Model::ModelNode::file) return;
        for (auto& c : n->kids)
        {
            PrintModelTree(c.get(), prefix);
        }
    }

    void Modify(ModifyStage stage)
    {
        bool prep = stage == Prepare;
        bool act = stage == MainAction;

        if (prep) out() << "Preparing action " << endl;
        if (act) out() << "Executing action " << endl;

        if (prep && printTreesBeforeAndAfter)
        {
            out() << " ---- local filesystem initial state ----" << endl;
            PrintLocalTree(fs::path(localTestBasePath()));
            out() << " ---- remote node tree initial state ----" << endl;
            Node* testRoot = client1().client.nodebyhandle(changeClient().basefolderhandle);
            if (Node* n = client1().drillchildnodebyname(testRoot, remoteTestBasePath))
            {
                PrintRemoteTree(n);
            }
        }

        switch (action)
        {
        case action_rename:
            if (prep)
            {
            }
            else if (act)
            {
                if (file)
                {
                    source_rename("f/f_0/file0_f_0", "file0_f_0_renamed", true, true, true);
                    destinationModel().emulate_rename("f/f_0/file0_f_0", "file0_f_0_renamed");
                }
                else
                {
                    source_rename("f/f_0", "f_0_renamed", true, true, false);
                    destinationModel().emulate_rename("f/f_0", "f_0_renamed");
                }
            }
            break;

        case action_moveWithinSync:
            if (prep)
            {
            }
            else if (act)
            {
                if (file)
                {
                    source_move("f/f_1/file0_f_1", "f/f_0", true, true, false);
                    destinationModel().emulate_move("f/f_1/file0_f_1", "f/f_0");
                }
                else
                {
                    source_move("f/f_1", "f/f_0", true, true, false);
                    destinationModel().emulate_move("f/f_1", "f/f_0");
                }
            }
            break;

        case action_moveOutOfSync:
            if (prep)
            {
            }
            else if (act)
            {
                if (file)
                {
                    source_move("f/f_0/file0_f_0", "outside", true, false, false);
                    destinationModel().emulate_delete("f/f_0/file0_f_0");
                }
                else
                {
                    source_move("f/f_0", "outside", true, false, false);
                    destinationModel().emulate_delete("f/f_0");
                }
            }
            break;

        case action_moveIntoSync:
            if (prep)
            {
            }
            else if (act)
            {
                if (file)
                {
                    source_move("outside/file0_outside", "f/f_0", true, false, false);
                    destinationModel().emulate_copy("outside/file0_outside", "f/f_0");
                }
                else
                {
                    source_move("outside", "f/f_0", true, false, false);
                    destinationModel().emulate_delete("f/f_0/outside");
                    destinationModel().emulate_copy("outside", "f/f_0");
                }
            }
            break;

        case action_delete:
            if (prep)
            {
            }
            else if (act)
            {
                if (file)
                {
                    source_delete("f/f_0/file0_f_0", true, true);
                    destinationModel().emulate_delete("f/f_0/file0_f_0");
                }
                else
                {
                    source_delete("f/f_0", true, true);
                    destinationModel().emulate_delete("f/f_0");
                }
            }
            break;

        default: ASSERT_TRUE(false);
        }
    }

    void CheckSetup(State&, bool initial)
    {
        if (!initial && printTreesBeforeAndAfter)
        {
            out() << " ---- local filesystem before change ----" << endl;
            PrintLocalTree(fs::path(localTestBasePath()));
            out() << " ---- remote node tree before change ----" << endl;
            Node* testRoot = client1().client.nodebyhandle(changeClient().basefolderhandle);
            if (Node* n = client1().drillchildnodebyname(testRoot, remoteTestBasePath))
            {
                PrintRemoteTree(n);
            }
        }

        if (!initial) out() << "Checking setup state (should be no changes in twoway sync source): "<< name() << endl;

        // confirm source is unchanged after setup  (Two-way is not sending changes to the wrong side)
        bool localfs = client1().confirmModel(backupId, localModel.findnode("f"), StandardClient::CONFIRM_LOCALFS, true); // todo: later enable debris checks
        bool localnode = client1().confirmModel(backupId, localModel.findnode("f"), StandardClient::CONFIRM_LOCALNODE, true); // todo: later enable debris checks
        bool remote = client1().confirmModel(backupId, remoteModel.findnode("f"), StandardClient::CONFIRM_REMOTE, true); // todo: later enable debris checks
        EXPECT_EQ(localfs, localnode);
        EXPECT_EQ(localnode, remote);
        EXPECT_TRUE(localfs && localnode && remote) << " failed in " << name();
    }


    // Two-way sync is stable again after the change.  Check the results.
    bool finalResult = false;
    void CheckResult(State&)
    {
        if (printTreesBeforeAndAfter)
        {
            out() << " ---- local filesystem after sync of change ----" << endl;
            PrintLocalTree(fs::path(localTestBasePath()));
            out() << " ---- remote node tree after sync of change ----" << endl;
            Node* testRoot = client1().client.nodebyhandle(changeClient().basefolderhandle);
            if (Node* n = client1().drillchildnodebyname(testRoot, remoteTestBasePath))
            {
                PrintRemoteTree(n);
            }
            out() << " ---- expected sync destination (model) ----" << endl;
            PrintModelTree(destinationModel().findnode("f"));
        }

        out() << "Checking twoway sync "<< name() << endl;
        bool localfs = client1().confirmModel(backupId, localModel.findnode("f"), StandardClient::CONFIRM_LOCALFS, true); // todo: later enable debris checks
        bool localnode = client1().confirmModel(backupId, localModel.findnode("f"), StandardClient::CONFIRM_LOCALNODE, true); // todo: later enable debris checks
        bool remote = client1().confirmModel(backupId, remoteModel.findnode("f"), StandardClient::CONFIRM_REMOTE, true); // todo: later enable debris checks
        EXPECT_EQ(localfs, localnode);
        EXPECT_EQ(localnode, remote);
        EXPECT_TRUE(localfs && localnode && remote) << " failed in " << name();
        finalResult = localfs && localnode && remote;
    }
};

void CatchupClients(StandardClient* c1, StandardClient* c2 = nullptr, StandardClient* c3 = nullptr)
{
    out() << "Catching up" << endl;
    auto pb1 = newPromiseBoolSP();
    auto pb2 = newPromiseBoolSP();
    auto pb3 = newPromiseBoolSP();
    if (c1) c1->catchup(pb1);
    if (c2) c2->catchup(pb2);
    if (c3) c3->catchup(pb3);
    ASSERT_TRUE((!c1 || pb1->get_future().get()) &&
                (!c2 || pb2->get_future().get()) &&
                (!c3 || pb3->get_future().get()));
    out() << "Caught up" << endl;
}

TEST(Sync, TwoWay_Highlevel_Symmetries)
{
    // confirm change is synced to remote, and also seen and applied in a second client that syncs the same folder
    fs::path localtestroot = makeNewTestRoot();

    StandardClient clientA1Steady(localtestroot, "clientA1S");
    StandardClient clientA1Resume(localtestroot, "clientA1R");
    StandardClient clientA2(localtestroot, "clientA2");
    ASSERT_TRUE(clientA1Steady.login_reset_makeremotenodes("MEGA_EMAIL", "MEGA_PWD", "twoway", 0, 0, true));
    ASSERT_TRUE(clientA1Resume.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD", false, true));
    ASSERT_TRUE(clientA2.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD", false, true));
    fs::create_directory(clientA1Steady.fsBasePath / fs::u8path("twoway"));
    fs::create_directory(clientA1Resume.fsBasePath / fs::u8path("twoway"));
    fs::create_directory(clientA2.fsBasePath / fs::u8path("twoway"));

    TwoWaySyncSymmetryCase::State allstate(clientA1Steady, clientA1Resume, clientA2);
    allstate.localBaseFolderSteady = clientA1Steady.fsBasePath / fs::u8path("twoway");
    allstate.localBaseFolderResume = clientA1Resume.fsBasePath / fs::u8path("twoway");

    std::map<std::string, TwoWaySyncSymmetryCase> cases;

    static string singleNamedTest = ""; // to investigate just one sync case, put its name here, otherwise we loop.

    for (int selfChange = 0; selfChange < 2; ++selfChange)
    {
        //if (!selfChange) continue;

        for (int up = 0; up < 2; ++up)
        {
            //if (!up) continue;

            for (int action = 0; action < (int)TwoWaySyncSymmetryCase::action_numactions; ++action)
            {
                //if (action != TwoWaySyncSymmetryCase::action_rename) continue;

                for (int file = 1; file < 2; ++file)
                {
                    //if (!file) continue;

                    for (int pauseDuringAction = 0; pauseDuringAction < 2; ++pauseDuringAction)
                    {
                        //if (pauseDuringAction) continue;

                        // we can't make changes if the client is not running
                        if (pauseDuringAction && selfChange) continue;

                        TwoWaySyncSymmetryCase testcase(allstate);
                        testcase.selfChange = selfChange != 0;
                        testcase.up = up;
                        testcase.action = TwoWaySyncSymmetryCase::Action(action);
                        testcase.file = file;
                        testcase.pauseDuringAction = pauseDuringAction;
                        testcase.printTreesBeforeAndAfter = !singleNamedTest.empty();

                        if (singleNamedTest.empty() || testcase.name() == singleNamedTest)
                        {
                            cases.emplace(testcase.name(), move(testcase));
                        }
                    }
                }
            }
        }
    }


    out() << "Creating initial local files/folders for " << cases.size() << " Two-way sync test cases" << endl;
    for (auto& testcase : cases)
    {
        testcase.second.SetupForSync();
    }

    // set up sync for A1, it should build matching cloud files/folders as the test cases add local files/folders
    handle backupId1 = clientA1Steady.setupSync_mainthread("twoway", "twoway");
    ASSERT_NE(backupId1, UNDEF);
    handle backupId2 = clientA1Resume.setupSync_mainthread("twoway", "twoway");
    ASSERT_NE(backupId2, UNDEF);
    assert(allstate.localBaseFolderSteady == clientA1Steady.syncSet(backupId1).localpath);
    assert(allstate.localBaseFolderResume == clientA1Resume.syncSet(backupId2).localpath);

    out() << "Full-sync all test folders to the cloud for setup" << endl;
    waitonsyncs(std::chrono::seconds(10), &clientA1Steady, &clientA1Resume);
    CatchupClients(&clientA1Steady, &clientA1Resume, &clientA2);
    waitonsyncs(std::chrono::seconds(20), &clientA1Steady, &clientA1Resume);

    out() << "Stopping full-sync" << endl;
    auto removeSyncByBackupId =
      [](StandardClient& sc, handle backupId)
      {
          bool removed = false;

          sc.client.syncs.removeSelectedSyncs(
            [&](SyncConfig& config, Sync*)
            {
                const bool matched = config.getBackupId() == backupId;
                removed |= matched;
                return matched;
            });

          return removed;
      };

    future<bool> fb1 = clientA1Steady.thread_do<bool>([&](StandardClient& sc, PromiseBoolSP pb) { pb->set_value(removeSyncByBackupId(sc, backupId1)); });
    future<bool> fb2 = clientA1Resume.thread_do<bool>([&](StandardClient& sc, PromiseBoolSP pb) { pb->set_value(removeSyncByBackupId(sc, backupId2)); });
    ASSERT_TRUE(waitonresults(&fb1, &fb2));

    out() << "Setting up each sub-test's Two-way sync of 'f'" << endl;
    for (auto& testcase : cases)
    {
        testcase.second.SetupTwoWaySync();
    }

    out() << "Letting all " << cases.size() << " Two-way syncs run" << endl;
    waitonsyncs(std::chrono::seconds(10), &clientA1Steady, &clientA1Resume);

    CatchupClients(&clientA1Steady, &clientA1Resume, &clientA2);

    out() << "Checking intial state" << endl;
    for (auto& testcase : cases)
    {
        testcase.second.CheckSetup(allstate, true);
    }


    // make changes in destination to set up test
    for (auto& testcase : cases)
    {
        testcase.second.Modify(TwoWaySyncSymmetryCase::Prepare);
    }

    CatchupClients(&clientA1Steady, &clientA1Resume, &clientA2);

    out() << "Letting all " << cases.size() << " Two-way syncs run" << endl;
    waitonsyncs(std::chrono::seconds(15), &clientA1Steady, &clientA1Resume, &clientA2);

    out() << "Checking Two-way source is unchanged" << endl;
    for (auto& testcase : cases)
    {
        testcase.second.CheckSetup(allstate, false);
    }

    // save session and local log out A1R to set up for resume
    string session;
    clientA1Resume.client.dumpsession(session);
    clientA1Resume.localLogout();

    int paused = 0;
    for (auto& testcase : cases)
    {
        if (testcase.second.pauseDuringAction)
        {
            ++paused;
        }
    }
    if (paused)
    {
        out() << "Paused " << paused << " Two-way syncs" << endl;
        WaitMillisec(1000);
    }

    out() << "Performing action " << endl;
    for (auto& testcase : cases)
    {
        testcase.second.Modify(TwoWaySyncSymmetryCase::MainAction);
    }
    waitonsyncs(std::chrono::seconds(15), &clientA1Steady, &clientA2);   // leave out clientA1Resume as it's 'paused' (locallogout'd) for now
    CatchupClients(&clientA1Steady, &clientA2);

    // resume A1R session (with sync), see if A2 nodes and localnodes get in sync again
    ASSERT_TRUE(clientA1Resume.login_fetchnodes(session));
    ASSERT_EQ(clientA1Resume.basefolderhandle, clientA2.basefolderhandle);

    int resumed = 0;
    for (auto& testcase : cases)
    {
        if (testcase.second.pauseDuringAction)
        {
            ++resumed;
        }
    }
    if (resumed)
    {
        out() << "Resumed " << resumed << " Two-way syncs" << endl;
        WaitMillisec(3000);
    }


    out() << "Letting all " << cases.size() << " Two-way syncs run" << endl;

    waitonsyncs(std::chrono::seconds(15), &clientA1Steady, &clientA1Resume, &clientA2);

    CatchupClients(&clientA1Steady, &clientA1Resume, &clientA2);

    out() << "Checking local and remote state in each sub-test" << endl;

    for (auto& testcase : cases)
    {
        testcase.second.CheckResult(allstate);
    }
    int succeeded = 0, failed = 0;
    for (auto& testcase : cases)
    {
        if (testcase.second.finalResult) ++succeeded;
        else
        {
            out() << "failed: " << testcase.second.name() << endl;
            ++failed;
        }
    }
    out() << "Succeeded: " << succeeded << " Failed: " << failed << endl;

    // Clear tree-state cache.
    {
        StandardClient cC(localtestroot, "cC");
        ASSERT_TRUE(cC.login_fetchnodes("MEGA_EMAIL", "MEGA_PWD", false, true));
    }
}

#endif
