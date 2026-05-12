#include "des_operator.h"

#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace {
const int IP[64] = {//初始置换表
    58, 50, 42, 34, 26, 18, 10, 2,
    60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6,
    64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17, 9, 1,
    59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5,
    63, 55, 47, 39, 31, 23, 15, 7};

const int IP_INV[64] = {//逆初始置换表
    40, 8, 48, 16, 56, 24, 64, 32,
    39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30,
    37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28,
    35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26,
    33, 1, 41, 9, 49, 17, 57, 25};

const int E[48] = {//选择扩展运算E盒
    32, 1, 2, 3, 4, 5,
    4, 5, 6, 7, 8, 9,
    8, 9, 10, 11, 12, 13,
    12, 13, 14, 15, 16, 17,
    16, 17, 18, 19, 20, 21,
    20, 21, 22, 23, 24, 25,
    24, 25, 26, 27, 28, 29,
    28, 29, 30, 31, 32, 1};

const int P[32] = {//置换运算P
    16, 7, 20, 21,
    29, 12, 28, 17,
    1, 15, 23, 26,
    5, 18, 31, 10,
    2, 8, 24, 14,
    32, 27, 3, 9,
    19, 13, 30, 6,
    22, 11, 4, 25};

const int PC1[56] = {//密钥置换选择1表
    57, 49, 41, 33, 25, 17, 9,
    1, 58, 50, 42, 34, 26, 18,
    10, 2, 59, 51, 43, 35, 27,
    19, 11, 3, 60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15,
    7, 62, 54, 46, 38, 30, 22,
    14, 6, 61, 53, 45, 37, 29,
    21, 13, 5, 28, 20, 12, 4};

const int PC2[48] = {//密钥置换选择2表
    14, 17, 11, 24, 1, 5,
    3, 28, 15, 6, 21, 10,
    23, 19, 12, 4, 26, 8,
    16, 7, 27, 20, 13, 2,
    41, 52, 31, 37, 47, 55,
    30, 40, 51, 45, 33, 48,
    44, 49, 39, 56, 34, 53,
    46, 42, 50, 36, 29, 32};

const int SHIFTS[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};//每轮密钥左移位数

const int S[8][4][16] = {//S盒，8个，每个4行16列
    {
        {14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7},
        {0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8},
        {4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0},
        {15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13},
    },
    {
        {15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10},
        {3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5},
        {0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15},
        {13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9},
    },
    {
        {10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8},
        {13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1},
        {13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7},
        {1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12},
    },
    {
        {7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15},
        {13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9},
        {10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4},
        {3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14},
    },
    {
        {2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9},
        {14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6},
        {4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14},
        {11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3},
    },
    {
        {12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11},
        {10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8},
        {9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6},
        {4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13},
    },
    {
        {4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1},
        {13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6},
        {1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2},
        {6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12},
    },
    {
        {13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7},
        {1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2},
        {7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8},
        {2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11},
    }};
}  // namespace

DesOperator::DesOperator(std::uint64_t key) : key_(0), subKeys_{} {
    setKey(key);
}

void DesOperator::setKey(std::uint64_t key) {
    key_ = key;
    generateSubKeys();
}

std::uint64_t DesOperator::permute(std::uint64_t input, const int* table, int tableSize, int inputBits) {//根据给定的置换表对输入进行置换
    std::uint64_t output = 0;//输出初始为0
    for (int i = 0; i < tableSize; ++i) {//遍历置换表
        output <<= 1;//输出左移1位，为下一位腾出空间
        int sourceIndex = inputBits - table[i];//计算输入中对应位的索引，注意输入位从1开始计数
        output |= (input >> sourceIndex) & 0x1ULL;//将输入中对应位的值取出，并放到输出的最低位
    }
    return output;
}

std::uint32_t DesOperator::leftRotate28(std::uint32_t value, int shift) {//对一个28位的值进行左旋转
    value &= 0x0FFFFFFF;//确保只处理28位
    return ((value << shift) | (value >> (28 - shift))) & 0x0FFFFFFF;//左旋转后再次确保结果仍然是28位
}

void DesOperator::generateSubKeys() {//生成16轮子密钥
    std::uint64_t key56 = permute(key_, PC1, 56, 64);//从64位密钥中选择56位，并进行置换
    std::uint32_t c = static_cast<std::uint32_t>((key56 >> 28) & 0x0FFFFFFF);//左半部分
    std::uint32_t d = static_cast<std::uint32_t>(key56 & 0x0FFFFFFF);//右半部分，28位

    for (int round = 0; round < 16; ++round) {//每轮进行左移，并根据PC2表选择48位作为子密钥
        c = leftRotate28(c, SHIFTS[round]);//左半部分左移
        d = leftRotate28(d, SHIFTS[round]);//右半部分左移
        std::uint64_t merged = (static_cast<std::uint64_t>(c) << 28) | d;//将左右半部分合并成56位
        subKeys_[round] = permute(merged, PC2, 48, 56);//根据PC2表选择48位作为子密钥，并存储
    }
}

std::uint32_t DesOperator::feistel(std::uint32_t right, std::uint64_t subKey) {//Feistel函数
    std::uint64_t expanded = permute(right, E, 48, 32);//将32位的右半部分扩展为48位
    std::uint64_t mixed = expanded ^ subKey;//与子密钥进行异或混合

    std::uint32_t sOutput = 0;//S盒替代，输出32位
    for (int i = 0; i < 8; ++i) {//将48位的混合结果分成8组，每组6位，输入到对应的S盒中进行替代
        std::uint8_t chunk = static_cast<std::uint8_t>((mixed >> (42 - i * 6)) & 0x3F);//取出当前组的6位
        int row = ((chunk & 0x20) >> 4) | (chunk & 0x01);
        int col = (chunk >> 1) & 0x0F;
        sOutput = (sOutput << 4) | static_cast<std::uint32_t>(S[i][row][col]);//将S盒的输出拼接成32位
    }

    return static_cast<std::uint32_t>(permute(sOutput, P, 32, 32));//最后进行P置换，得到Feistel函数的输出
}

std::uint64_t DesOperator::encryptBlock(std::uint64_t plaintext) const {//对一个64位的明文块进行加密
    std::uint64_t ip = permute(plaintext, IP, 64, 64);//初始置换
    std::uint32_t left = static_cast<std::uint32_t>(ip >> 32);//左半部分
    std::uint32_t right = static_cast<std::uint32_t>(ip & 0xFFFFFFFF);//右半部分

    for (int round = 0; round < 16; ++round) {//16轮迭代
        std::uint32_t nextLeft = right;//下一轮的左半部分是当前的右半部分
        std::uint32_t nextRight = left ^ feistel(right, subKeys_[round]);//下一轮的右半部分是当前的左半部分与Feistel函数输出的异或
        left = nextLeft;//更新当前的左半部分
        right = nextRight;//更新当前的右半部分
    }

    std::uint64_t preOutput = (static_cast<std::uint64_t>(right) << 32) | left;//合并左右半部分，注意此时左右半部分的位置已经交换
    return permute(preOutput, IP_INV, 64, 64);//逆初始置换，得到最终的密文块
}

std::uint64_t DesOperator::decryptBlock(std::uint64_t ciphertext) const {//对一个64位的密文块进行解密
    std::uint64_t ip = permute(ciphertext, IP, 64, 64);//初始置换
    std::uint32_t left = static_cast<std::uint32_t>(ip >> 32);//左半部分
    std::uint32_t right = static_cast<std::uint32_t>(ip & 0xFFFFFFFF);//右半部分

    for (int round = 15; round >= 0; --round) {//16轮迭代，子密钥的使用顺序与加密相反
        std::uint32_t nextLeft = right;//下一轮的左半部分是当前的右半部分
        std::uint32_t nextRight = left ^ feistel(right, subKeys_[round]);//下一轮的右半部分是当前的左半部分与Feistel函数输出的异或
        left = nextLeft;//更新当前的左半部分
        right = nextRight;//更新当前的右半部分
    }

    std::uint64_t preOutput = (static_cast<std::uint64_t>(right) << 32) | left;//合并左右半部分，注意此时左右半部分的位置已经交换
    return permute(preOutput, IP_INV, 64, 64);//逆初始置换，得到最终的明文块
}

std::vector<std::uint8_t> DesOperator::encrypt(const std::string& plaintext) const {//对一个字符串进行加密
    std::vector<std::uint8_t> bytes(plaintext.begin(), plaintext.end());

    std::uint8_t pad = static_cast<std::uint8_t>(8 - (bytes.size() % 8));
    if (pad == 0) {
        pad = 8;
    }
    bytes.insert(bytes.end(), pad, pad);

    std::vector<std::uint8_t> encrypted;
    encrypted.reserve(bytes.size());

    for (size_t i = 0; i < bytes.size(); i += 8) {//每8个字节作为一个块进行加密
        std::uint64_t block = 0;
        for (int j = 0; j < 8; ++j) {
            block = (block << 8) | bytes[i + j];
        }
        std::uint64_t cipher = encryptBlock(block);
        for (int j = 7; j >= 0; --j) {//将加密后的块转换回字节，并添加到结果中
            encrypted.push_back(static_cast<std::uint8_t>((cipher >> (j * 8)) & 0xFF));
        }//注意加密后的块是64位的，转换回字节时需要从高位开始取
    }

    return encrypted;//返回加密后的字节数组
}

std::string DesOperator::decrypt(const std::vector<std::uint8_t>& ciphertext) const {//对一个加密的字节数组进行解密
    if (ciphertext.empty() || ciphertext.size() % 8 != 0) {
        throw std::runtime_error("Ciphertext length must be a positive multiple of 8");
    }

    std::vector<std::uint8_t> plainBytes;//存储解密后的字节数组
    plainBytes.reserve(ciphertext.size());//解密后的字节数不会超过加密前的字节数

    for (size_t i = 0; i < ciphertext.size(); i += 8) {
        std::uint64_t block = 0;
        for (int j = 0; j < 8; ++j) {
            block = (block << 8) | ciphertext[i + j];
        }

        std::uint64_t plain = decryptBlock(block);
        for (int j = 7; j >= 0; --j) {
            plainBytes.push_back(static_cast<std::uint8_t>((plain >> (j * 8)) & 0xFF));
        }
    }

    std::uint8_t pad = plainBytes.back();
    if (pad == 0 || pad > 8 || pad > plainBytes.size()) {
        throw std::runtime_error("Invalid PKCS#7 padding");
    }
    for (size_t i = plainBytes.size() - pad; i < plainBytes.size(); ++i) {
        if (plainBytes[i] != pad) {
            throw std::runtime_error("Invalid PKCS#7 padding");
        }
    }
    plainBytes.resize(plainBytes.size() - pad);

    return std::string(plainBytes.begin(), plainBytes.end());//将解密后的字节数组转换回字符串，并返回
}

std::string DesOperator::bytesToHex(const std::vector<std::uint8_t>& bytes) {//将字节数组转换为十六进制字符串
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (std::uint8_t b : bytes) {
        oss << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
}

std::vector<std::uint8_t> DesOperator::hexToBytes(const std::string& hex) {//将十六进制字符串转换为字节数组
    if (hex.size() % 2 != 0) {
        throw std::runtime_error("Hex string length must be even");
    }

    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);

    for (size_t i = 0; i < hex.size(); i += 2) {
        if (!std::isxdigit(static_cast<unsigned char>(hex[i])) || !std::isxdigit(static_cast<unsigned char>(hex[i + 1]))) {
            throw std::runtime_error("Invalid hex string");
        }
        std::uint8_t value = static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16));
        out.push_back(value);
    }

    return out;
}