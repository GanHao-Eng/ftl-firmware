/**
 * @file utils.c
 * @brief 工具函数实现
 * @details 通用工具函数实现，包括内存安全操作、字符串处理、
 *          延时函数、CRC校验和版本信息管理
 */

#include "utils.h"
#include <string.h>
#include <stdio.h>

/* ============================================================
 *  CRC32 查表法
 * ============================================================ */

/**
 * @brief CRC32 查找表（多项式 0xEDB88320，即标准以太网CRC32）
 * @details 预计算的256项查找表，用于加速CRC32计算
 *          生成多项式：x^32 + x^26 + x^23 + x^22 + x^16 + x^12 + x^11 +
 *                      x^10 + x^8 + x^7 + x^5 + x^4 + x^2 + x + 1
 */
static const uint32_t g_crc32_table[256] = {
    0x00000000U, 0x77073096U, 0xEE0E612CU, 0x990951BAU,
    0x076DC419U, 0x706AF48FU, 0xE963A535U, 0x9E6495A3U,
    0x0EDB8832U, 0x79DCB8A4U, 0xE0D5E91EU, 0x97D2D988U,
    0x09B64C2BU, 0x7EB17CBDU, 0xE7B82D07U, 0x90BF1D91U,
    0x1DB71064U, 0x6AB020F2U, 0xF3B97148U, 0x84BE41DEU,
    0x1ADAD47DU, 0x6DDDE4EBU, 0xF4D4B551U, 0x83D385C7U,
    0x136C9856U, 0x646BA8C0U, 0xFD62F97AU, 0x8A65C9ECU,
    0x14015C4FU, 0x63066CD9U, 0xFA0F3D63U, 0x8D080DF5U,
    0x3B6E20C8U, 0x4C69105EU, 0xD56041E4U, 0xA2677172U,
    0x3C03E4D1U, 0x4B04D447U, 0xD20D85FDU, 0xA50AB56BU,
    0x35B5A8FAU, 0x42B2986CU, 0xDBBBC9D6U, 0xACBCF940U,
    0x32D86CE3U, 0x45DF5C75U, 0xDCD60DCFU, 0xABD13D59U,
    0x26D930ACU, 0x51DE003AU, 0xC8D75180U, 0xBFD06116U,
    0x21B4F4B5U, 0x56B3C423U, 0xCFBA9599U, 0xB8BDA50FU,
    0x2802B89EU, 0x5F058808U, 0xC60CD9B2U, 0xB10BE924U,
    0x2F6F7C87U, 0x58684C11U, 0xC1611DABU, 0xB6662D3DU,
    0x76DC4190U, 0x01DB7106U, 0x98D220BCU, 0xEFD5102AU,
    0x71B18589U, 0x06B6B51FU, 0x9FBFE4A5U, 0xE8B8D433U,
    0x7807C9A2U, 0x0F00F934U, 0x9609A88EU, 0xE10E9818U,
    0x7F6A0DBBU, 0x086D3D2DU, 0x91646C97U, 0xE6635C01U,
    0x6B6B51F4U, 0x1C6C6162U, 0x856530D8U, 0xF262004EU,
    0x6C0695EDU, 0x1B01A57BU, 0x8208F4C1U, 0xF50FC457U,
    0x65B0D9C6U, 0x12B7E950U, 0x8BBEB8EAU, 0xFCB9887CU,
    0x62DD1DDFU, 0x15DA2D49U, 0x8CD37CF3U, 0xFBD44C65U,
    0x4DB26158U, 0x3AB551CEU, 0xA3BC0074U, 0xD4BB30E2U,
    0x4ADFA541U, 0x3DD895D7U, 0xA4D1C46DU, 0xD3D6F4FBU,
    0x4369E96AU, 0x346ED9FCU, 0xAD678846U, 0xDA60B8D0U,
    0x44042D73U, 0x33031DE5U, 0xAA0A4C5FU, 0xDD0D7CC9U,
    0x5005713CU, 0x270241AAU, 0xBE0B1010U, 0xC90C2086U,
    0x5768B525U, 0x206F85B3U, 0xB966D409U, 0xCE61E49FU,
    0x5EDEF90EU, 0x29D9C998U, 0xB0D09822U, 0xC7D7A8B4U,
    0x59B33D17U, 0x2EB40D81U, 0xB7BD5C3BU, 0xC0BA6CADU,
    0xEDB88320U, 0x9ABFB3B6U, 0x03B6E20CU, 0x74B1D29AU,
    0xEAD54739U, 0x9DD277AFU, 0x04DB2615U, 0x73DC1683U,
    0xE3630B12U, 0x94643B84U, 0x0D6D6A3EU, 0x7A6A5AA8U,
    0xE40ECF0BU, 0x9309FF9DU, 0x0A00AE27U, 0x7D079EB1U,
    0xF00F9344U, 0x8708A3D2U, 0x1E01F268U, 0x6906C2FEU,
    0xF762575DU, 0x806567CBU, 0x196C3671U, 0x6E6B06E7U,
    0xFED41B76U, 0x89D32BE0U, 0x10DA7A5AU, 0x67DD4ACCU,
    0xF9B9DF6FU, 0x8EBEEFF9U, 0x17B7BE43U, 0x60B08ED5U,
    0xD6D6A3E8U, 0xA1D1937EU, 0x38D8C2C4U, 0x4FDFF252U,
    0xD1BB67F1U, 0xA6BC5767U, 0x3FB506DDU, 0x48B2364BU,
    0xD80D2BDAU, 0xAF0A1B4CU, 0x36034AF6U, 0x41047A60U,
    0xDF60EFC3U, 0xA867DF55U, 0x316E8EEFU, 0x4669BE79U,
    0xCB61B38CU, 0xBC66831AU, 0x256FD2A0U, 0x5268E236U,
    0xCC0C7795U, 0xBB0B4703U, 0x220216B9U, 0x5505262FU,
    0xC5BA3BBEU, 0xB2BD0B28U, 0x2BB45A92U, 0x5CB36A04U,
    0xC2D7FFA7U, 0xB5D0CF31U, 0x2CD99E8BU, 0x5BDEAE1DU,
    0x9B64C2B0U, 0xEC63F226U, 0x756AA39CU, 0x026D930AU,
    0x9C0906A9U, 0xEB0E363FU, 0x72076785U, 0x05005713U,
    0x95BF4A82U, 0xE2B87A14U, 0x7BB12BAEU, 0x0CB61B38U,
    0x92D28E9BU, 0xE5D5BE0DU, 0x7CDCEFB7U, 0x0BDBDF21U,
    0x86D3D2D4U, 0xF1D4E242U, 0x68DDB3F8U, 0x1FDA836EU,
    0x81BE16CDU, 0xF6B9265BU, 0x6FB077E1U, 0x18B74777U,
    0x88085AE6U, 0xFF0F6A70U, 0x66063BCAU, 0x11010B5CU,
    0x8F659EFFU, 0xF862AE69U, 0x616BFFD3U, 0x166CCF45U,
    0xA00AE278U, 0xD70DD2EEU, 0x4E048354U, 0x3903B3C2U,
    0xA7672661U, 0xD06016F7U, 0x4969474DU, 0x3E6E77DBU,
    0xAED16A4AU, 0xD9D65ADCU, 0x40DF0B66U, 0x37D83BF0U,
    0xA9BCAE53U, 0xDEBB9EC5U, 0x47B2CF7FU, 0x30B5FFE9U,
    0xBDBDF21CU, 0xCABAC28AU, 0x53B39330U, 0x24B4A3A6U,
    0xBAD03605U, 0xCDD70693U, 0x54DE5729U, 0x23D967BFU,
    0xB3667A2EU, 0xC4614AB8U, 0x5D681B02U, 0x2A6F2B94U,
    0xB40BBE37U, 0xC30C8EA1U, 0x5A05DF1BU, 0x2D02EF8DU
};

/* ============================================================
 *  版本信息
 * ============================================================ */

/**
 * @brief 固件版本信息全局实例
 * @details 编译时自动填充构建日期和时间，用于版本追踪和固件升级判断
 */
static const firmware_version_t g_version = {
    .major = 1,           ///< 主版本号（不兼容修改时递增）
    .minor = 0,           ///< 次版本号（功能新增时递增）
    .patch = 0,           ///< 修订号（Bug修复时递增）
    .build = 1,           ///< 构建号（每次构建递增）
    .name = "FTL Firmware", ///< 固件名称
    .date = __DATE__,     ///< 编译日期（由预处理器自动填充）
    .time = __TIME__      ///< 编译时间（由预处理器自动填充）
};

/* ============================================================
 *  内存操作工具
 * ============================================================ */

/**
 * @brief 安全内存拷贝（带边界检查）
 * @param[out] dst 目标缓冲区指针
 * @param[in] dst_size 目标缓冲区大小（字节）
 * @param[in] src 源数据指针
 * @param[in] src_size 源数据大小（字节）
 * @retval RET_OK 拷贝成功
 * @retval RET_ERR_PARAM 参数错误（空指针）
 * @retval RET_ERR_OVERWRITE 目标缓冲区不足，防止缓冲区溢出
 * @details 高安全内存拷贝，在执行memcpy前检查目标缓冲区大小，
 *          防止缓冲区溢出漏洞。这是CERT C安全编码标准的推荐做法。
 */
ret_code_t utils_memcpy_safe(void *dst, uint32_t dst_size, const void *src, uint32_t src_size)
{
    /* 空指针检查 */
    if (dst == NULL || src == NULL) {
        return RET_ERR_PARAM;
    }

    /* 边界检查：目标缓冲区必须能容纳源数据 */
    if (dst_size < src_size) {
        return RET_ERR_OVERWRITE;
    }

    /* 执行内存拷贝 */
    memcpy(dst, src, src_size);

    return RET_OK;
}

/**
 * @brief 安全字符串拷贝（带边界检查和自动终止）
 * @param[out] dst 目标字符串缓冲区
 * @param[in] dst_size 目标缓冲区大小（字节，包含终止符空间）
 * @param[in] src 源字符串指针
 * @retval RET_OK 拷贝成功
 * @retval RET_ERR_PARAM 参数错误（空指针）
 * @retval RET_ERR_OVERWRITE 目标缓冲区不足
 * @details 安全字符串拷贝，确保目标字符串始终以'\0'终止，
 *          防止字符串溢出和未终止字符串导致的安全问题。
 */
ret_code_t utils_strcpy_safe(char *dst, uint32_t dst_size, const char *src)
{
    uint32_t src_len = 0;

    /* 空指针检查 */
    if (dst == NULL || src == NULL) {
        return RET_ERR_PARAM;
    }

    /* 计算源字符串长度（限制在目标缓冲区大小内） */
    src_len = utils_strnlen(src, dst_size);

    /* 检查是否有足够空间存放字符串和终止符 */
    if (src_len >= dst_size) {
        return RET_ERR_OVERWRITE;
    }

    /* 拷贝字符串内容 */
    memcpy(dst, src, src_len);

    /* 确保字符串以'\0'终止 */
    dst[src_len] = '\0';

    return RET_OK;
}

/**
 * @brief 安全字符串长度计算（限制最大长度）
 * @param[in] str 字符串指针
 * @param[in] max_len 最大扫描长度
 * @return 字符串长度（不超过max_len）
 * @details 计算字符串长度，但最多扫描max_len个字节，
 *          防止对未终止字符串的无限扫描导致的越界访问。
 */
uint32_t utils_strnlen(const char *str, uint32_t max_len)
{
    uint32_t len = 0;

    /* 空指针返回0 */
    if (str == NULL) {
        return 0U;
    }

    /* 扫描字符串，直到遇到'\0'或达到最大长度 */
    while (len < max_len && str[len] != '\0') {
        len++;
    }

    return len;
}

/* ============================================================
 *  时间工具
 * ============================================================ */

/**
 * @brief 毫秒级延时
 * @param[in] ms 延时毫秒数
 * @note 简化实现，使用空循环模拟延时。实际固件中应使用硬件定时器
 *       或RTOS的延时函数，以避免CPU忙等和延时不准确的问题。
 */
void utils_delay_ms(uint32_t ms)
{
    volatile uint32_t i = 0;
    volatile uint32_t j = 0;

    /* 外层循环：毫秒计数 */
    for (i = 0; i < ms; i++) {
        /* 内层循环：模拟1毫秒的空循环（约1000次迭代） */
        for (j = 0; j < 1000U; j++) {
            /* 空循环延时，volatile防止编译器优化掉 */
        }
    }
}

/**
 * @brief 微秒级延时
 * @param[in] us 延时微秒数
 * @note 简化实现，使用空循环模拟延时。实际精度取决于CPU主频，
 *       固件应使用硬件定时器或高精度延时函数。
 */
void utils_delay_us(uint32_t us)
{
    volatile uint32_t i = 0;

    /* 空循环模拟微秒级延时 */
    for (i = 0; i < us; i++) {
        /* 空循环延时，volatile防止编译器优化掉 */
    }
}

/* ============================================================
 *  CRC 工具
 * ============================================================ */

/**
 * @brief 计算CRC32校验值（查表法）
 * @param[in] data 数据缓冲区指针
 * @param[in] len 数据长度（字节）
 * @return CRC32校验值（32位）
 * @details 使用标准以太网CRC32算法（多项式0xEDB88320），
 *          采用查表法加速计算。初始值0xFFFFFFFF，结果取反。
 *          广泛用于数据完整性校验、ZIP、PNG等格式。
 */
uint32_t utils_crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFU;  /* CRC初始值（全1） */
    uint32_t i = 0;

    /* 空指针或零长度返回0 */
    if (data == NULL || len == 0U) {
        return 0U;
    }

    /* 逐字节计算CRC，使用查找表加速 */
    for (i = 0; i < len; i++) {
        /* 低8位与数据异或作为表索引，查表后与CRC高24位异或 */
        crc = g_crc32_table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8);
    }

    /* 最终结果取反 */
    return crc ^ 0xFFFFFFFFU;
}

/**
 * @brief 计算CRC16校验值（Modbus RTU标准）
 * @param[in] data 数据缓冲区指针
 * @param[in] len 数据长度（字节）
 * @return CRC16校验值（16位）
 * @details 使用Modbus RTU标准CRC16算法（多项式0xA001，即0x8005的反转），
 *          初始值0xFFFF。逐位计算，适用于工业通信、Modbus协议等场景。
 */
uint16_t utils_crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFFU;  /* CRC初始值（全1） */
    uint32_t i = 0;
    uint32_t j = 0;

    /* 空指针或零长度返回0 */
    if (data == NULL || len == 0U) {
        return 0U;
    }

    /* 逐字节计算 */
    for (i = 0; i < len; i++) {
        /* 数据字节与CRC低8位异或 */
        crc ^= (uint16_t)data[i];

        /* 逐位处理（8位） */
        for (j = 0; j < 8U; j++) {
            /* 如果最低位为1，右移后异或多项式0xA001 */
            if (crc & 0x0001U) {
                crc = (crc >> 1) ^ 0xA001U;
            } else {
                /* 最低位为0，直接右移 */
                crc >>= 1;
            }
        }
    }

    return crc;
}

/* ============================================================
 *  版本信息接口
 * ============================================================ */

/**
 * @brief 获取固件版本信息
 * @return 固件版本信息结构体指针
 * @details 返回只读的版本信息结构体，包含主版本、次版本、修订号、
 *          构建号、固件名称、编译日期和时间。用于版本查询和
 *          固件升级兼容性判断。
 */
const firmware_version_t *utils_get_version(void)
{
    return &g_version;
}

/**
 * @brief 打印固件版本信息到控制台
 * @details 以格式化方式打印固件名称、版本号、构建号和编译时间，
 *          用于启动时的版本显示和调试信息输出。
 */
void utils_print_version(void)
{
    /* 打印版本横幅 */
    printf("========================================\n");

    /* 打印固件名称和版本号 */
    printf("  %s v%u.%u.%u (build %u)\n",
           g_version.name,
           g_version.major,
           g_version.minor,
           g_version.patch,
           g_version.build);

    /* 打印构建日期和时间 */
    printf("  Build: %s %s\n", g_version.date, g_version.time);
    printf("========================================\n");
}
