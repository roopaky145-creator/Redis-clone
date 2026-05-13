#include "../include/RespParser.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

void testCompleteArray() {
    RespParser parser;
    std::string payload = "*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$4\r\nJohn\r\n";
    parser.append(payload);
    
    auto result = parser.parseOne();
    assert(result.status == ParseStatus::Complete);
    assert(result.value.type == RespType::Array);
    assert(result.value.elements.size() == 3);
    assert(result.value.elements[0].text == "SET");
    assert(result.value.elements[1].text == "name");
    assert(result.value.elements[2].text == "John");
    std::cout << "[PASS] Complete Array Parse\n";
}

void testPartialRead() {
    RespParser parser;
    std::string chunk1 = "*2\r\n$3\r\nGE";
    std::string chunk2 = "T\r\n$4\r\nname\r\n";
    
    parser.append(chunk1);
    auto result1 = parser.parseOne();
    assert(result1.status == ParseStatus::Incomplete);
    
    parser.append(chunk2);
    auto result2 = parser.parseOne();
    assert(result2.status == ParseStatus::Complete);
    assert(result2.value.type == RespType::Array);
    assert(result2.value.elements.size() == 2);
    assert(result2.value.elements[0].text == "GET");
    std::cout << "[PASS] Partial Read Parse\n";
}

int main() {
    std::cout << "Running RESP Parser Tests...\n";
    testCompleteArray();
    testPartialRead();
    std::cout << "All tests passed!\n";
    return 0;
}
