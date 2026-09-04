#!/usr/bin/env python3
"""更新简历和README中的项目名"""

# 1. 更新简历
resume_path = r'C:\Users\甘昊\Doubao\chats\2026-07-28\new-chat\简历-甘昊-SSD固件方向.md'
with open(resume_path, 'r', encoding='utf-8') as f:
    content = f.read()

# 更新项目标题
content = content.replace(
    '### SSD固件参考架构：FTL算法栈与NVMe/TCP+UFS双协议栈实现（ftl-firmware）',
    '### ftl-firmware：SSD固件参考实现——FTL算法栈与NVMe/TCP+UFS双协议栈'
)
print('[OK] 简历项目标题更新')

# 更新个人优势中的项目名
content = content.replace(
    '独立完成SSD固件参考架构（ftl-firmware）',
    '独立完成ftl-firmware（SSD固件参考实现）'
)
print('[OK] 简历个人优势项目名更新')

# 更新项目描述中的"参考架构"
content = content.replace(
    '独立设计并实现一套完整的SSD固件参考架构',
    '独立设计并实现一套完整的SSD固件参考实现'
)
print('[OK] 简历项目描述更新')

with open(resume_path, 'w', encoding='utf-8') as f:
    f.write(content)

# 2. 更新README
readme_path = r'E:\Learn\Code\ftl-firmware\README.md'
with open(readme_path, 'r', encoding='utf-8') as f:
    content = f.read()

# 更新标题
content = content.replace(
    '# SSD固件参考架构：FTL算法栈与NVMe/TCP+UFS双协议栈实现',
    '# ftl-firmware：SSD固件参考实现——FTL算法栈与NVMe/TCP+UFS双协议栈'
)
print('[OK] README标题更新')

# 更新描述中的"参考架构"
content = content.replace(
    '本项目是一套完整的SSD固件参考架构',
    '本项目是一套完整的SSD固件参考实现'
)
print('[OK] README描述更新')

with open(readme_path, 'w', encoding='utf-8') as f:
    f.write(content)

print('\n项目名更新完成!')
