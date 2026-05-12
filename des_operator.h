#pragma once

#include <cstdint>
#include <string>
#include <vector>

class DesOperator {
public:
    explicit DesOperator(std::uint64_t key = 0);//实现构造函数，接受一个64位的密钥，默认值为0

    void setKey(std::uint64_t key);//实现setKey方法，设置密钥并生成子密钥
    std::uint64_t encryptBlock(std::uint64_t plaintext) const;//实现encryptBlock方法，对一个64位的明文块进行加密，返回64位的密文块
    std::uint64_t decryptBlock(std::uint64_t ciphertext) const;//实现decryptBlock方法，对一个64位的密文块进行解密，返回64位的明文块

    std::vector<std::uint8_t> encrypt(const std::string& plaintext) const;//实现encrypt方法，对一个字符串进行加密，返回加密后的字节数组
    std::string decrypt(const std::vector<std::uint8_t>& ciphertext) const;//实现decrypt方法，对一个加密的字节数组进行解密，返回解密后的字符串

    static std::string bytesToHex(const std::vector<std::uint8_t>& bytes);//实现bytesToHex方法，将字节数组转换为十六进制字符串
    static std::vector<std::uint8_t> hexToBytes(const std::string& hex);//实现hexToBytes方法，将十六进制字符串转换为字节数组

private:
    std::uint64_t key_;//存储原始密钥
    std::uint64_t subKeys_[16];//存储16轮的子密钥

    void generateSubKeys();//实现generateSubKeys方法，根据原始密钥生成16轮的子密钥
    static std::uint32_t feistel(std::uint32_t right, std::uint64_t subKey);//实现feistel方法，Feistel函数，接受右半部分和子密钥，返回32位的输出

    static std::uint64_t permute(std::uint64_t input, const int* table, int tableSize, int inputBits);//实现permute方法，根据给定的置换表对输入进行置换，返回置换后的结果
    static std::uint32_t leftRotate28(std::uint32_t value, int shift);//实现leftRotate28方法，对一个28位的值进行左旋转，接受值和旋转位数，返回旋转后的结果
};