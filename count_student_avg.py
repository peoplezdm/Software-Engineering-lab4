p = 1000000007

# 输入参与方id
id = int(input("请输入参与方id: "))

# Student_id读取属于自己的秘密份额：student_1_id.txt, student_2_id.txt, student_3_id.txt
data = []
for i in range(1, 4):
    with open(f'student_{i}_{id}.txt', 'r') as f:
        # 打开文本
        data.append(int(f.read()))  # 读取文本

# 计算三个秘密份额的和
d = 0
for i in range(0, 3):
    d = (d + data[i]) % p

# 将求和后的秘密份额保存到文件d_id.txt内
with open(f'd_{id}.txt', 'w') as f:
    f.write(str(d))

print(f'Student_{id}的秘密份额和已保存到d_{id}.txt')
