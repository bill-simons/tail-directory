// tailer.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <filesystem>
#include <iostream>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <utility>
#include <set>
#include <algorithm>
#include <regex>
#include <atomic>
#include <Windows.h>
#include <conio.h>
#include <stdlib.h>
#include <wchar.h>
#include <process.h>
#include <signal.h>
#include "Args.h"
#include "unique_handle.h"

namespace fs = std::experimental::filesystem::v1;

///////////////////////////////////////////////////////////////////////////////
// forward declarations
//
class LogFileInfo;
struct GlobalData;
struct GenericHandlePolicy;
std::string get_last_error();
std::string & trim(std::string & str);
int64_t filetime_to_unix_time(FILETIME &fileTime);

///////////////////////////////////////////////////////////////////////////////
// typedefs
//
typedef std::list<std::shared_ptr<LogFileInfo>> LogFileInfoList;
typedef std::unordered_map <std::string, std::shared_ptr<LogFileInfo>> PrefixLogFileInfoMap;
typedef std::shared_ptr<unique_handle<GenericHandlePolicy>> SharedUniqueFileHandlePtr;

///////////////////////////////////////////////////////////////////////////////
// constants
//

/** read line max buffer size*/
const std::streamsize BUFLEN{ 4096 };

/** polling inerval */
DWORD  POLLING_INTERVAL_MILLIS{ 750 };

/** signal flags for worker thread used in FileMonitorData struct */
const int DIRECTORY_MODIFIED = 0x1000;
const int STOP_MONITORING    = 0x4000;

///////////////////////////////////////////////////////////////////////////////
// global data
//

/**
 * Global object used to pass signals from main thread to worker thread
 * and by interrupt handlers to clean up on exit.
 */
 std::atomic<GlobalData *> pGlobalData{nullptr};


///////////////////////////////////////////////////////////////////////////////
// classes /structs
//

/**
 * Global data class used by the worker thread that polls for changed files.
 */
struct GlobalData {
   std::atomic<int> signal{0};    // signal from main thread to worker thread

   // directory change handle
   HANDLE directoryMonitorHandle{INVALID_HANDLE_VALUE};

   GlobalData(HANDLE h) : directoryMonitorHandle{h} {}
   ~GlobalData() { closeDirectoryMonitorHandle();  }
   void closeDirectoryMonitorHandle() {
      if (directoryMonitorHandle != NULL) {
         FindCloseChangeNotification(directoryMonitorHandle);
         directoryMonitorHandle = NULL;
      }
   }
};

// Information passed from the main to the file monitor thread
struct Options {
   fs::path    logdir;
   std::regex  filename_regex;
   std::regex  beep_regex;
   bool        beepOnException;
   unsigned    max_files;
   Options(fs::path &path, std::regex &frx, std::regex &brx, bool beep, unsigned max)
   : logdir{ path }, filename_regex{ frx }, beep_regex{ brx }, beepOnException{beep}, max_files{max}
   {
   }
};

/**
  Policy object for unique_handle when dealing with generic handle returned from
  CreateFile or any other call that uses the CloseHandle call to dispose.
*/
struct GenericHandlePolicy {
   typedef HANDLE handle_type;
   static void close(handle_type handle) {
      if (handle != NULL && handle != INVALID_HANDLE_VALUE) {
         CloseHandle(handle);
      }
   }
   static handle_type get_null() { return NULL; }
   static bool is_null(handle_type handle) {  return handle == NULL; }
};

/**
 * Information about a file being monitored: the path, date, file size, last-tailed position
 */
class LogFileInfo {
private:
   std::string prefix{ "" };
   fs::path path{ "" };
   int64_t create_time{0};
   int64_t write_time{0};
   int64_t file_size{0};
   int64_t last_tailed_pos{0};

public:
   LogFileInfo() {
   }

   LogFileInfo(const LogFileInfo &other) :
         prefix{other.getPrefix()},
         path{other.getPath()},
         create_time{other.getCreateTime() },
         write_time{other.getWriteTime()},
         file_size{ other.getFileSize() },
         last_tailed_pos{ other.getLastTailedPosition()}
   {
   }

   LogFileInfo(std::string prefixStr, fs::path filePath) :
         prefix(prefixStr),
         path(filePath)
   {
      LPCWSTR pathStr = filePath.c_str();
      WIN32_FILE_ATTRIBUTE_DATA fileData;
      if (GetFileAttributesEx(pathStr, GetFileExInfoStandard, &fileData)) {
         create_time = filetime_to_unix_time(fileData.ftCreationTime);
         write_time = filetime_to_unix_time(fileData.ftLastWriteTime);
         LARGE_INTEGER lint;
         lint.HighPart = fileData.nFileSizeHigh;
         lint.LowPart = fileData.nFileSizeLow;
         file_size = lint.QuadPart;
         last_tailed_pos = lint.QuadPart;
      }
   }

   void startWatching() {
      // if it's a new file it might already have data in it by the time we first see it,
      // but we want to start tailing from the start
      std::string rewind_message;
      if (file_size > 0 && file_size < 1000 ) {
         std::time_t now = std::time(nullptr);
         std::time_t create = getCreateTime()/1000;  // create time is milliseconds, not seconds
         if ((now - create) < 6) {
            setLastTailedPosition(0);
            rewind_message = " (rewinding to start of file)";
         }
      }
      std::cout << "********* " << prefix << ": WATCHING " << path.filename() << rewind_message << std::endl;
   }
   void stopWatching() {
      std::cout << "********* STOPPING " << path.filename() << std::endl;
   }

   std::string getPrefix() const { return prefix; }
   fs::path getPath() const { return path; }
   int64_t getCreateTime() const { return create_time; }
   int64_t getWriteTime() const { return write_time; }
   int64_t getFileSize() const { return file_size; }
   int64_t getLastTailedPosition() const { return last_tailed_pos; }
   void setWriteTime(int64_t wt) {
      write_time = wt;
   }
   void setFileSize(int64_t size) {
      file_size = size;
   }
   void setLastTailedPosition(int64_t pos) {
      last_tailed_pos = pos;
   }
};

/**
 * Command-line argument parser
 */
class Args {
private:
   args::ArgumentParser parser;
   args::HelpFlag help;
   args::Positional<std::string> dir;
   args::ValueFlag<std::string> file_pattern;
   args::ValueFlag<std::string> line_beep_pattern;
   args::Flag nobeep;
   args::ValueFlag<int> max_files;
   int stat{0};

public:
   Args(int argc, char *argv[]) :
         parser("Monitor a directory for the newest files that match a pattern and print any lines added to those files on the console. Optionally emit a beep when a line contains a matching 'beep pattern'."),
         help(parser, "help", "Display this help menu", {'h', "help"}),
         dir(parser, "directory", "The directory to search for log files. Files whose name matches the 'pattern' regular expression will be monitored."),
         file_pattern(parser, "pattern", "Regex for matching file names. The identifier that uniquely identifies each file type must be enclosed in parenthesis as the first capturing group.",
                      {'p', "pattern"}),
         line_beep_pattern(parser, "pattern", "Regex that triggers a beep when an output line matches.", {'b', "beep"}),
         nobeep(parser, "nobeep", "Disable checking for the 'beep' regular expression.", {'n', "nobeep"}),
         max_files(parser, "max_files", "Maximum number of files to match", {'m', "max"})
   {
      try {
         parser.ParseCLI(argc, argv);
      } catch (args::Help) {
         showHelp();
      } catch (args::ParseError e) {
         std::cerr << e.what() << std::endl;
         std::cerr << parser;
         stat = -1;
      } catch (args::ValidationError e) {
         std::cerr << e.what() << std::endl;
         std::cerr << parser;
         stat = -1;
      }
   }

   int getStat() {
      return stat;
   }

   bool getHelp() {
      return help ? true : false;
   }

   void showHelp() {
      std::cout << parser;
   }
   std::string getDir() {
      return dir ? args::get(dir) : "";
   }
   std::string getFilePattern() {
      return file_pattern ? args::get(file_pattern) : "";
   }
   std::string getBeepPattern() {
      return line_beep_pattern ? args::get(line_beep_pattern) : "";
   }
   bool getBeep() {
      return nobeep ? false : true;
   }
   int getMaxFiles() {
      return max_files ? args::get(max_files) : 10;
   }
};


///////////////////////////////////////////////////////////////////////////////
// utility functions
//

int64_t filetime_to_unix_time(FILETIME &fileTime) {
   //Get the number of seconds since January 1, 1970 12:00am UTC
   const int64_t UNIX_TIME_START = 0x019DB1DED53E8000; // January 1, 1970 (start of Unix epoch) in "ticks"
   const int64_t TICKS_PER_SECOND = 10000;             // a tick is 10 ms

   //Copy the low and high parts of FILETIME into a LARGE_INTEGER
   //This is so we can access the full 64-bits as an Int64 without causing an alignment fault
   LARGE_INTEGER li;
   li.LowPart = fileTime.dwLowDateTime;
   li.HighPart = fileTime.dwHighDateTime;

   //Convert ticks since 1/1/1970 into seconds
   return (li.QuadPart - UNIX_TIME_START) / TICKS_PER_SECOND;
}

std::string & ltrim(std::string & str) {
   auto it2 = std::find_if(str.begin(), str.end(), [](char ch) { return !std::isspace<char>(ch, std::locale::classic()); });
   str.erase(str.begin(), it2);
   return str;
}

std::string & rtrim(std::string & str) {
   auto it1 = std::find_if(str.rbegin(), str.rend(), [](char ch) { return !std::isspace<char>(ch, std::locale::classic()); });
   str.erase(it1.base(), str.end());
   return str;
}

std::string & trim(std::string & str) {
   return ltrim(rtrim(str));
}

std::string get_last_error() {
   std::ostringstream sstr;
   DWORD err = GetLastError();
   sstr << "0x" << std::hex << std::setfill('0') << std::setw(8) << err;
   if (err != 0) {
      LPSTR messageBuffer = nullptr;
      size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
         NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);
      
      std::string message(messageBuffer, size);
      sstr << " (" << trim(message) << ")";
      LocalFree(messageBuffer);
   }
   return sstr.str();
}

SharedUniqueFileHandlePtr open_file_handle(fs::path path) {
   SharedUniqueFileHandlePtr sharedHandle;
   HANDLE hFile = CreateFile(path.c_str(), 0,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
   if (hFile != INVALID_HANDLE_VALUE) {
      unique_handle<GenericHandlePolicy> *pUH = new unique_handle<GenericHandlePolicy>(hFile);
      sharedHandle.reset(pUH);
   }
   return sharedHandle;
}


///////////////////////////////////////////////////////////////////////////////
// program code
//

std::shared_ptr<PrefixLogFileInfoMap> collectLogFiles(fs::path &logdir, std::regex &filename_regex) {
   PrefixLogFileInfoMap *pmap = new PrefixLogFileInfoMap{100};
   std::unordered_map<std::string, std::shared_ptr<LogFileInfoList>> prefixToFileListPtrMap{100};
   std::smatch match;
   for (const auto &entry : fs::directory_iterator(logdir)) {
      const fs::path path = entry.path();
      if (fs::exists(path) && !fs::is_directory(path)) {
         fs::path filename = path.filename();
         std::string str = filename.string();

         if (std::regex_search(str, match, filename_regex)) {
            std::string prefix = match[1];
            if (!prefix.empty()) {
               std::shared_ptr<LogFileInfoList> listPtr;
               auto entryIt = prefixToFileListPtrMap.find(prefix);
               if(entryIt != prefixToFileListPtrMap.end()) {
                  listPtr = entryIt->second;
               } else {
                  LogFileInfoList *fileList = new LogFileInfoList();
                  listPtr = std::shared_ptr<LogFileInfoList>(fileList);
                  prefixToFileListPtrMap.emplace(prefix, listPtr);
               }
               std::shared_ptr<LogFileInfo> ptr(new LogFileInfo(prefix, path));
               listPtr->push_back(ptr);
            }
         }
      }
   }

   // sort the list of files that match each prefix by their timestamp
   // add the most recent one to the output map
   for(auto entry : prefixToFileListPtrMap) {
      std::string prefix = entry.first;
      std::shared_ptr<LogFileInfoList> value = entry.second;
      if (!value->empty()) {
         value->sort([](std::shared_ptr<LogFileInfo> pList0, std::shared_ptr<LogFileInfo> pList1) {
            return pList0->getCreateTime() > pList1->getCreateTime();   // largest (most-recent) to smallest
         });

         std::shared_ptr<LogFileInfo> pinfo = value->front();
         pmap->emplace(prefix, pinfo);
      }
   }

   return std::shared_ptr<PrefixLogFileInfoMap>(pmap);
}

void showTooManyFilesMessage(std::shared_ptr<PrefixLogFileInfoMap> pmap, unsigned max_files) {
   std::cout << "Too many files match the given pattern (maximum number of files is " << max_files << ", use the -m option to increase the limit)." << std::endl;
   std::cout << std::setw(25) << std::left << "Unique Prefix" << " : " << std::setw(50) << "File Name" << std::endl;
   std::cout << std::setw(25) << std::left << "==================" << " : " << "=================================================" << std::endl;
   for(auto entry : *pmap) {
      auto prefix = entry.first;
      auto pinfo = entry.second;
      auto filename = pinfo->getPath().filename();
      std::cout << std::setw(25) << std::left << prefix << " : " << std::setw(50) << filename << std::endl;
   }
}

std::shared_ptr<PrefixLogFileInfoMap> collectInitialLogFiles(fs::path &logdir, std::regex &filename_regex, unsigned max_files) {
   std::shared_ptr<PrefixLogFileInfoMap> pmap = collectLogFiles(logdir, filename_regex);
   if (pmap) {
      if(pmap->size() <= max_files) {
         std::cout << "Press CTRL-C to exit." << std::endl;
         if (pmap->empty()) {
            std::cout << "********* WARNING: no files found that match the file name regular expression." << std::endl;
         } else {
            for (auto entry : *pmap) {
               entry.second->startWatching();
            }
         }
      } else {
         showTooManyFilesMessage(pmap, max_files);
         pmap.reset();
      }
   }
   return pmap;
}

void updateLogFilesMap(std::shared_ptr<PrefixLogFileInfoMap> oldMap, std::shared_ptr<PrefixLogFileInfoMap> newMap, unsigned max_files) {
   std::set<std::string> oldKeys;
   std::set<std::string> newKeys;
   std::set<std::string> removed;
   std::transform(oldMap->begin(), oldMap->end(),std::inserter(oldKeys, oldKeys.end()), [](auto pair) { return pair.first; });
   std::transform(newMap->begin(), newMap->end(), std::inserter(newKeys, newKeys.end()), [](auto pair) { return pair.first; });
   std::set_difference(oldKeys.begin(),oldKeys.end(),newKeys.begin(),newKeys.end(),std::inserter(removed,removed.end()));
   for (auto oldKey : removed) {
      auto pOldValue = oldMap->at(oldKey);
      pOldValue->stopWatching();
      oldMap->erase(oldKey);
   }
   for (auto newEntry : *newMap) {
      auto oldEntryIt = oldMap->find(newEntry.first);
      if (oldEntryIt == oldMap->end()) {
         if (oldMap->size() < max_files) {
            oldMap->emplace(newEntry);
            newEntry.second->startWatching();
         }
         else {
            std::cout << "********* Maximum number of files are being monitored ("  << max_files << "). Not watching new file " << newEntry.second->getPath().filename() << std::endl;
         }
      }
      else {
         std::string prefix = oldEntryIt->first;
         auto oldFilePath = oldEntryIt->second->getPath();
         auto newFilePath = newEntry.second->getPath();
         if (oldFilePath.compare(newFilePath) != 0) {
            oldEntryIt->second->stopWatching();
            newEntry.second->startWatching();
            oldMap->insert_or_assign(prefix, newEntry.second);
         }
      }
   }
}

void tailOneFile(LogFileInfo &info, int64_t fileSize, int64_t writeTime, std::regex *pbeep_regex) {
   int64_t prevSize = info.getFileSize();
   if ((writeTime != info.getWriteTime()) || (fileSize != prevSize)) {
      if (fileSize < prevSize) {
         // file size has shrunk -- start tailing from new end of file
         info.setLastTailedPosition(info.getFileSize());
      } else if(fileSize > prevSize) {
         // data has been added to the file
         std::ifstream ifs(info.getPath().c_str());
         int64_t last_tail_pos = info.getLastTailedPosition();
         ifs.seekg(last_tail_pos, std::ios_base::beg);
         if (ifs.good()) {
            std::unique_ptr<char> buf(new char[BUFLEN]);
            std::streamsize pos = (std::streamsize)last_tail_pos;
            char *pbuf = buf.get();
            while (pos < fileSize) {
               std::fill(pbuf,pbuf+BUFLEN,0);
               ifs.getline(pbuf, BUFLEN);
               std::streampos current_pos = ifs.tellg();
               if (current_pos > pos) {
                  pos = current_pos;
                  if (!ifs.fail() && !ifs.eof()) {
                     last_tail_pos = current_pos;
                     std::string current_line(pbuf);
                     std::cout << info.getPrefix() << ": " << current_line << std::endl;
                     if (pbeep_regex != nullptr) {
                        std::smatch match;
                        if (std::regex_search(current_line, match, *pbeep_regex)) {
                           Beep(500, 500);     // MessageBeep(MB_OK)  would add dependency on User32.dll, so far we only have depenencies on Kernel32.dll
                        }
                     }
                  }
               }
               else {
                  break;
               }
            }
         }
         info.setLastTailedPosition(last_tail_pos);
      }

      info.setFileSize(fileSize);
      info.setWriteTime(writeTime);
   }
}

void tailAllFiles(std::shared_ptr<PrefixLogFileInfoMap> pmap, std::regex *pbeep_regex) {
   for (auto entry : *pmap) {
      std::string prefix = entry.first;
      auto pinfo = entry.second;
      SharedUniqueFileHandlePtr hPtr = open_file_handle(pinfo->getPath());
      if (hPtr) {
         HANDLE h = hPtr->get();
         FILETIME fileTime;
         LARGE_INTEGER liSize;
         bool bTime = GetFileTime(h, NULL, NULL, &fileTime);
         bool bSize = GetFileSizeEx(h, &liSize);
         hPtr.reset();
         if (bTime && bSize) {
            int64_t fileSize = liSize.QuadPart;
            int64_t writeTime = filetime_to_unix_time(fileTime);
            tailOneFile(*pinfo, fileSize, writeTime, pbeep_regex);
         }
         else {
            std::cout << "********* " << prefix << ": Cannot get file time and/or size" << std::endl;
         }
      } else {
         std::cout << "********* " << prefix << ": Unable to open file handle" << std::endl;
      }
   }
}

unsigned __stdcall workerThreadProc(void* userData) {
   // worker thread that runs a polling loop looking for changes in the files
   // being monitored, and that updates the list of monitored files when the
   // main thread signals that the directoy has changed.

   Options *pdata = (Options*)userData;

   // copy data to local variables just in case the data object goes out of scope
   // (main thread exits early)
   fs::path   logdir = pdata->logdir;
   std::regex filename_regex = pdata->filename_regex;
   std::regex beep_regex = pdata->beep_regex;
   std::regex *pbeep_regex = pdata->beepOnException ? &(beep_regex) : nullptr;
   int max_files = pdata->max_files;
   DWORD millis = POLLING_INTERVAL_MILLIS;

   std::shared_ptr<PrefixLogFileInfoMap> pmap = collectInitialLogFiles(logdir,filename_regex,max_files);
   if(pmap) {
      while (pGlobalData.load() != nullptr) {
         int signal = pGlobalData.load()->signal.exchange(0);
         if ((signal & STOP_MONITORING) != 0) {
            break;
         }
         if ((signal & DIRECTORY_MODIFIED) != 0) {
            std::shared_ptr<PrefixLogFileInfoMap> pNewMap = collectLogFiles(logdir, filename_regex);
            updateLogFilesMap(pmap, pNewMap, max_files);
         }
         tailAllFiles(pmap,pbeep_regex);
         GlobalData *p = pGlobalData.load();
         if ((p == nullptr || (p->signal.load() & STOP_MONITORING) != 0)) {
            break;
         }
         Sleep(millis);
      }
   }
   return 0;
}

int mainThreadProc(Options *pOptions) {
   // main thread that starts the file monitor worker thread running and then 
   // waits to be notified of changes to the monitored directory. When notified
   // it signals the worker thread to update the list of monitored files.

   // The polling thread is necessary because the directory change notification
   // (FindFirstChangeNotification) with FILE_NOTIFY_CHANGE_LAST_WRITE doesn't
   // send notifications immediately when a file is changed due to write buffering.
   // Changes are sent only when the buffer is written to disk.  With polling,
   // the write buffer is flushed to disk when a new handle is opened on the file.

   int stat = 0;
   LPCWSTR path = pOptions->logdir.c_str();
   HANDLE hDirMonitor = FindFirstChangeNotification(path, false, FILE_NOTIFY_CHANGE_FILE_NAME);
   pGlobalData.store(new GlobalData(hDirMonitor));
   if (hDirMonitor == NULL || hDirMonitor == INVALID_HANDLE_VALUE) {
      stat = 4;
      std::cout << "Unable to monitor directory for changes: " << get_last_error() << std::endl;
   } else {
      _beginthreadex_proc_type runnable = &workerThreadProc;
      uintptr_t pollingThreadHandle = _beginthreadex(nullptr,0,runnable,pOptions,0,nullptr);
      if (pollingThreadHandle != -1) {
         HANDLE hPollingThread = (HANDLE)pollingThreadHandle;  // beginThread ultimately calls the OS CreateThread so the handles are compatible with the Wait functions
         HANDLE pHandles[2]{ hDirMonitor,hPollingThread };
         while (true) {
            int stat = WaitForMultipleObjects(2, pHandles, false, INFINITE);
            if (stat == WAIT_OBJECT_0) {
               pGlobalData.load()->signal |= DIRECTORY_MODIFIED;
               if (!FindNextChangeNotification(hDirMonitor)) {
                  stat = 5;
                  std::cout << "********* FindNextChangeNotification failed.  Error=" << get_last_error() << std::endl;
                  break;
               }
            }
            else if (stat == (WAIT_OBJECT_0 + 1)) {
               // worker thread has exited
               break;
            }
            else {
               // not expecting to get a  WAIT_TIMEOUT since we specified an infinite wait
               stat = 6;
               std::cout << "********* Unknown WaitForSingleObject result=" << stat << std::endl;
               break;
            }
         }
         pGlobalData.load()->signal.store(STOP_MONITORING);
         WaitForSingleObject(hPollingThread,2000);
      }
      else {
         std::cout << "Unable to start file monitoring thread: " << get_last_error() << std::endl;
      }
   }

   GlobalData *p = pGlobalData.exchange(nullptr);
   delete p;   // closes the FindFirstChangeNotification handle
   return stat;
}

BOOL WINAPI windowsCtrlHandler(DWORD fdwCtrlType) {
   switch (fdwCtrlType) {
      // Handle the CTRL-C signal.
   case CTRL_C_EVENT:
   case CTRL_CLOSE_EVENT:
   case CTRL_BREAK_EVENT:
   case CTRL_LOGOFF_EVENT:
   case CTRL_SHUTDOWN_EVENT:
      std::cout << "********* Shutdown in CTRL-C handler" << std::endl;
      if (pGlobalData.load() != nullptr) {
         pGlobalData.load()->signal.store(STOP_MONITORING);
         pGlobalData.load()->closeDirectoryMonitorHandle();
      }
      return TRUE;
   default:
      return FALSE;
   }
}

void signalHandler(int s) {
   std::cout << "********* Shutdown in signal handler" << std::endl;
   if (pGlobalData.load() != nullptr) {
      pGlobalData.load()->signal.store(STOP_MONITORING);
      pGlobalData.load()->closeDirectoryMonitorHandle();
   }
   exit(0);
}

bool installExitHandlers() {
   bool stat = false;
   if (SetConsoleCtrlHandler(windowsCtrlHandler, TRUE)) {
      signal(SIGINT,&signalHandler);
      stat = true;
   }
   return stat;
}

int main(int argc, char *argv[]) {
   int stat = 0;
   Args args(argc, argv);
   auto logdir = fs::path(args.getDir());
   if (args.getStat() != 0) {
      return args.getStat();
   } else if (args.getHelp()) {
      // no-op
   } else if (logdir.empty()) {
      args.showHelp();
   } else if (!fs::is_directory(logdir)) {
      stat = 1;
      std::cout << "Not a directory: " << logdir << std::endl;
   } else {
      std::string line_pat = args.getFilePattern();
      std::string beep_pat = args.getBeepPattern();
      bool beepOnException = args.getBeep();
      if (line_pat.empty()) line_pat = "(tfe.*)_\\d+\\.log";
      if (beep_pat.empty()) beep_pat = ".*[a-zA-Z]+\\.[a-zA-Z]+(Exception|Error):";   // something.somethingError:  or something.somethingException:
      std::cout << "Scanning directory:   " << logdir << std::endl;
      std::cout << "File name regex:      " << line_pat << std::endl;
      if (beepOnException) {
         std::cout << "Beep if line matches: " << beep_pat << std::endl;
      }
      try {
         std::regex filename_regex(line_pat);
         std::regex beep_regex(beep_pat);
         if (installExitHandlers()) {
            unsigned maxFiles = (unsigned)args.getMaxFiles();
            Options options{logdir, filename_regex, beep_regex, beepOnException, maxFiles};
            stat = mainThreadProc(&options);
         }
         else {
            stat = 3;
            std::cout << "Unable to add shutdown hook" << std::endl;
            std::cout << "Last Error=" << get_last_error() << std::endl;
         }
      }
      catch (std::regex_error e) {
         stat = 2;
         std::cout << "Invalid pattern: " << e.what() << std::endl;
      }
   }
   return stat;
}
