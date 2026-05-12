import ss_function_avg as ss_f

这里是B2分支

p = 1000000007

# 随机选取两个参与方，例如student2和student3，获得d2, d3，从而恢复出d=a+b+c
# 读取d2, d3
d_23 = []
for i in range(2, 4):
    with open(f'd_{i}.txt', 'r') as f:
        # 打开文本
        d_23.append(int(f.read()))  # 读取文本

# 加法重构获得d
d = ss_f.restructure_polynomial([2, 3], d_23, 2, p)
print(f'恢复出的总和为：{d}')

# 计算平均值：avg = d / 3 = d * inv(3) (mod p)
inv_3 = ss_f.mod_inverse(3, p)
avg = (d * inv_3) % p

print(f'得数据平均值为：{avg}')

这里是c4分支