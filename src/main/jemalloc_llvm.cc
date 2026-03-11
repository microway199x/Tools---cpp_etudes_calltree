//
// Created by grakra on 25-7-10.
//
#include "llvm/Support/MemoryBuffer.h"
#include <optional>
#include <iostream>
int main(){
    auto s = (char*)std::malloc(14);
    memcpy(s, "hello world!", 13);

    std::optional<llvm::Align> align = llvm::Align(16);
    auto a = llvm::WritableMemoryBuffer::getNewUninitMemBuffer(4096, "test_buffer", align);
    std::cout<<a.get()<<std::endl;
    std::cout<<s<<std::endl;
    std::cout<<a->getBufferSize()<<std::endl;
    std::cout<<"start="<<a->getBufferStart() << std::endl;
    std::cout<<"end="<<a->getBufferEnd() << std::endl;
}