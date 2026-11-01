#include "FileStamp.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace {
void require(bool value,const char* why){if(!value)throw std::runtime_error{why};}
void write(const std::filesystem::path& path,const char* text){std::ofstream out{path,std::ios::binary|std::ios::trunc};out<<text;}
}
int main() {
    try {
        const auto path=std::filesystem::temp_directory_path()/("sm2vr-stamp-"+std::to_string(GetCurrentProcessId())+".txt");
        std::filesystem::remove(path);
        FileStamp stamp;
        require(stamp.changed(path),"the first poll loads the file, even a missing one");
        require(!stamp.changed(path),"a file that is still missing is unchanged");
        write(path,"capsort\n");
        require(stamp.changed(path),"a file that appears is a change");
        require(!stamp.changed(path) && !stamp.changed(path),"an untouched file is not reloaded");
        write(path,"native\n");
        require(stamp.changed(path),"a rewrite of another size is a change");
        std::filesystem::last_write_time(path,std::filesystem::last_write_time(path)+std::chrono::seconds(5));
        require(stamp.changed(path),"a rewrite of the same size with a new write time is a change");
        stamp.forget();require(stamp.changed(path),"forget makes the next poll load the file again");
        std::filesystem::remove(path);
        require(stamp.changed(path) && !stamp.changed(path),"a file that disappears is one change");
        std::cout<<"file stamp tests passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
