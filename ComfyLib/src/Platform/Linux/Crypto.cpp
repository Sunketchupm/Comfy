#include "IO/Crypto/Crypto.h"
#include <openssl/evp.h>
#include <limits>

namespace Comfy::IO::Crypto
{
    namespace
    {
        bool Decrypt(const EVP_CIPHER* cipher, const u8* input, u8* output, size_t size,
                     const std::array<u8, KeySize>& key, const u8* iv)
        {
            if (size % 16 != 0 || size > static_cast<size_t>(std::numeric_limits<int>::max()))
                return false;
            if (size == 0)
                return true;
            if (input == nullptr || output == nullptr)
                return false;
            std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context(
                EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
            if (!context || EVP_DecryptInit_ex(context.get(), cipher, nullptr, key.data(), iv) != 1)
                return false;
            EVP_CIPHER_CTX_set_padding(context.get(), 0);
            int count = 0;
            int finalCount = 0;
            return EVP_DecryptUpdate(context.get(), output, &count, input, static_cast<int>(size)) == 1
                && EVP_DecryptFinal_ex(context.get(), output + count, &finalCount) == 1
                && static_cast<size_t>(count + finalCount) == size;
        }
    }
    bool DecryptAesEcb(const u8* input, u8* output, size_t size, const std::array<u8, KeySize>& key)
    {
        return Decrypt(EVP_aes_128_ecb(), input, output, size, key, nullptr);
    }
    bool DecryptAesCbc(const u8* input, u8* output, size_t size,
                     const std::array<u8, KeySize>& key, const std::array<u8, IVSize>& iv)
    {
        return Decrypt(EVP_aes_128_cbc(), input, output, size, key, iv.data());
    }
}
