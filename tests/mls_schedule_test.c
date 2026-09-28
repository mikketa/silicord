/* The MLS key schedule: the IETF mls-implementations key-schedule vectors for ciphersuite 2 (five epochs). */
#include <windows.h>
#include "test.h"
#include "mls_schedule.h"
#include "sha2.h"

typedef struct {
    const char *commit_secret;
    const char *psk_secret;
    const char *tree_hash;
    const char *confirmed_transcript_hash;
    const char *group_context;
    const char *joiner_secret;
    const char *welcome_secret;
    const char *init_secret;
    const char *sender_data_secret;
    const char *encryption_secret;
    const char *exporter_secret;
    const char *epoch_authenticator;
    const char *external_secret;
    const char *confirmation_key;
    const char *membership_key;
    const char *resumption_psk;
    const char *external_pub;
    const char *exp_label, *exp_context, *exp_secret;
    int exp_length;
} epoch_vector_t;

static const char k_group_id[] = "a897b53575b4dd35fed4466e4e714bfa949eaa72e616a9c68a47b39cb7a60d2e";
static const char k_initial_init[] = "a897b53575b4dd35fed4466e4e714bfa949eaa72e616a9c68a47b39cb7a60d2e";

static const epoch_vector_t k_epochs[] = {
    {"a22606222e350fd7f0937168fe7548fb06626ab143cba7611d641693b1447509",
     "e871b247379522395689182736cb3d1e7b108d6ae934b802223975de8dc3f80b",
     "9769e302a99c457350a8e636009b12a2fee068664004606d6318eb3a1977d818",
     "5e57c9364dc71f0f71b19ffe561ab77257c490708a47e29f8f73f2b318201d2f",
     "0001000220a897b53575b4dd35fed4466e4e714bfa949eaa72e616a9c68a47b39cb7a60d2e0000000000000000209769e302a99c457350a8e636009b12a2fee068664004606d6318eb3a1977d818205e57c9364dc71f0f71b19ffe561ab77257c490708a47e29f8f73f2b318201d2f00",
     "b3e94856e4ac46ce5a92176b7a1d97e1b8c5e4d50aff1bb25c7387b756dafd52",
     "159dc69117edda2a8d7f01f7b3ee9252a576e38e3524ebf8dbd59efa39ef3332",
     "8669c65d78b3f0ffea3d0e5d2205eb799ec39b97dada7624af0ccc3fa65bcd60",
     "a19b19d68dd5b2744bdd38e9576d8b68e7592395dcd9a19f8853d831cbb783bd",
     "de53a6bb794b7652cb69589cfae9007a8f6f50347d91821a02c02c94b8d502e9",
     "37b4777fbfa0bb7b83d5a4e52d1d249000564918b96436704595c36b57c1e366",
     "6bb5c0d569550a2c7e1917b0ebeef193b703281fc5eaa3392b3125c8e394f69d",
     "1f0124fdbb08c0d72123838e3731ab6953a53d565496f523bb3a4e539c949aac",
     "9d8dd78766b9391995f0cd122bff536c01e056ae3c29fbd8dca3a128f1a097ba",
     "136eed7ac9f27002cbb4bc47d91676341df8c8eaa8349828dd51d22d5ffb3f43",
     "99444cb54d7e6d7da939124f5a666bac5a878eb756405a437893914b33353504",
     "0404c9d6889bc661210f8c74d9e07e355755268949b606694c395516b978e76e88d30be4f47e427abf0e64126dee99df85a62a90de44fc7feafab3e355d6338d8c",
     "9ba13d54ecdec7cbefcb47b4268d7b1990fabc6d6e67681e167959389d84e4e4",
     "884f1af892ab002f5be4c5d5081ade9e0e6418c6ea7a9a92e90534f19dcef785",
     "fb21b093a965ca191ddc236dd38791d738d37818d8d3861020151dfdd8ee70b0",
     32},
    {"7b3027aa5d2224aab7e2a18660bbf57930e2e21d95e02b849c704d970e3e28c5",
     "ca7a68f2a8a52147d70f1eb7195de968d2e182b93596bc5a61393861e91180e4",
     "826a4d3b0956277ce5e272e4d18fdca023ffb63ea4cea636e34cc837ae7c5c5d",
     "14a2985ea47db0685924a74d47ac8a08ec241f843b536dd1348e3ffb2d78184e",
     "0001000220a897b53575b4dd35fed4466e4e714bfa949eaa72e616a9c68a47b39cb7a60d2e000000000000000120826a4d3b0956277ce5e272e4d18fdca023ffb63ea4cea636e34cc837ae7c5c5d2014a2985ea47db0685924a74d47ac8a08ec241f843b536dd1348e3ffb2d78184e00",
     "56da58805b3d101e29d2e30a289521f569022caf35b4d38dabf87c6fbca14a50",
     "3a41beb4e9c73e9f08e827bad80623118cd7ea7dc386d036950026b99e77fce3",
     "7e865d2798f8c4d525aaf16ca07ae590dded972a0d66003c8182470241c38dcf",
     "299b5fc2f3388c3d36fad329aa6e127e10cb75a9e087dd092c93c496ceea1c5d",
     "e99cd6a6fea96eaf2ec1deeb36b715070259330bcfe6c3f8d126fc532150c5ee",
     "14cbe123c10819429719541776782030dd46fc5467186f8d47e13d71facaba44",
     "35304d2ca84327c0d857539619966f721ea62666f2f6ed54905bebf7e958cc25",
     "e33af6fd60347fdfb08c645acbb5230ebb7fe45f86d41bf9d6854d5921c6617d",
     "2f04b94ed0bbe4b2af50a0ef14437e9d6ae241b9d184c7b49d0f4f6904dab072",
     "50da02ce823804e3fe58293d3386bea7f32360c9644c2383965e6f7ff6a1a663",
     "cda55479f2d79d2fd3cc31a5b088524f8e14e1d09a6c452f7ad4e5447b5a57bb",
     "04cd06e3f46cb57f8616cf06cf3c9ea317cf80db4e93da2e2dfdf1a4fd5f4e80636a4c87cb170418d70adb0fbd5ce3b31af1eb00047b2892e7ec951d3496552272",
     "ed66d7f1da52171ac9448f0f902edcfefa4ebbda843a43bd3d173cb7c5b4331e",
     "02dc18fc5bc4d9093cf41fa0053521653775b123784d40ac7d46cc5a72ef4d46",
     "71f2a450aa47dccbe2984c4d741e5c4c637630175962ce5f2eff66b4c26bb953",
     32},
    {"d2825785628f1ea7404d6761f27272af5f99416ea28cc9d335df47ed2b0097d4",
     "599aa672406270914c60d30b7a31d2f2e217c3b5298b279b79e34c65a60e5f24",
     "661ce3bd9ebec8608fa97bf5413a4588f50a8f9face225ec67a6d29c862b2516",
     "b5d7ec8c9d8b6a28c9467fe4918844be6acc08e98c1e10c71122e95f9a5e78c5",
     "0001000220a897b53575b4dd35fed4466e4e714bfa949eaa72e616a9c68a47b39cb7a60d2e000000000000000220661ce3bd9ebec8608fa97bf5413a4588f50a8f9face225ec67a6d29c862b251620b5d7ec8c9d8b6a28c9467fe4918844be6acc08e98c1e10c71122e95f9a5e78c500",
     "fe900e89cfd3efb2389bcb745ee9aa0891c28251d04276c7c1025672b9389a33",
     "d285a6878ef5333c3ceda57a20092e367df7561329ee6960feda21b374647a26",
     "6ca0cb77418803ed69a79b38ecad9fd23023b65e7b5dc76eb3e57de7f4d66fc6",
     "980a117d376d79a89dbe6213e438fe6835bb7c051b74c700555a492c503b44db",
     "b60eb28fffd896fb3e2d50ae005adeae99752dca0deeec04615264179d4f5841",
     "c54d18dab31f33c601e9c118bf4ed7da3f6ba47d7015ab75ea0cbed9e04e1307",
     "41075a99af8ad0f536344699d21cc759582fec96c0dddf880dfc9dbb3f8b890c",
     "3e9d1f5432b348921d433e4a0fa7688211efa2d8a29538129fa3a04e3ccae08c",
     "ac9a97807e7167765187e26a3ba52e6d20e104740baacfd0e809bf6a94cdfda7",
     "58cd8e9842b6088f8525554102f4ff6831f31b75060920422c85c1dfe54c21b0",
     "06efd465e3dfdafa94dd9e936c7f71512ea3b1f3dc53368579b4d607bdb4b5b9",
     "04d291538b8844075474030538a927a26fdccb86955b7b014178626fb6aeda43d397e0fd89080cb8216c2d8bdf353791ea8eccf25cd7d2aa90f6272eebb1737c3f",
     "06f549e9bf966d7b7135b6ef6d4e3032a1c720adf0281c3ef1c0beac0da88621",
     "b0fa7e3f0f2199278a55267d551d43946bbfc6d847632867dd86abd1217982a5",
     "5b197935caefa88ed8d8205af646b522a02a6c1b488395621236ab1b5a0f378d",
     32},
    {"f652baa9151c9719ecb2716240a2a5ed9aeede1df19de0de862ded166a724783",
     "4106e07ffe8f0bfdbfd317d92e37a1fc6c4d1fba53ee054b7acf8587013d533b",
     "08225ed7f3b0b8aa9f03b24395ed8ee7002d38209fddd7d941dd8ac629ac8a62",
     "dab8c2f8ec97a0e5a137c55a5b9ac1ccdf5ae8329810e98e0bc3930aae0b4be1",
     "0001000220a897b53575b4dd35fed4466e4e714bfa949eaa72e616a9c68a47b39cb7a60d2e00000000000000032008225ed7f3b0b8aa9f03b24395ed8ee7002d38209fddd7d941dd8ac629ac8a6220dab8c2f8ec97a0e5a137c55a5b9ac1ccdf5ae8329810e98e0bc3930aae0b4be100",
     "6624cd191a3141784d05038ba45ddd94b35be850cde61e173f2dc3bbe85dc156",
     "744595bf2df5d81a75a6e8ad626b850c59e29400541839fd7e60d6b9d94bb767",
     "254f4d6e5ac4d01faa9b765aeb6cc2035dd7030a54716309076aa4bf0e82c9e8",
     "4ec712ba8a2d5ed0325fa0752179c4b68ee6654460aab1d7c7d8ea5ce2abbdc5",
     "2c7c2ef55a2c03fbbf7196e4a51f2a70feb473c1e5c5b7891222e3c7cac76aba",
     "ac5e044dfb6dac6e4d5ed1c24d3704ffcd7122d3728a718dfc3d0806a42be7b9",
     "3dee04aa653c5587f2a745b65ef7c6d99e484741aa0897915431bb919005c5a4",
     "994c525395647e95fd7bb97811caf64a5905ff68e38eb779d54eb9e8ac3a2fa8",
     "5b46056ff41d185365c5bd7e864047183b245c739939403cec96c0c8265a6fe0",
     "0ff704006401de7487bd9d97856825daa5a3d4ee385155e8e105e5ad88b88e78",
     "767c53ad0a962f8d80e8cca8b6a75a9d4a70bff81c55dfc7c19f264aaaad4a99",
     "04f98e5fe952473e144e348d3fd45c8c9a84a3a64030520eaa244c78e32408d62d5ae6bd9b3ead1bddaf214862c9a4945cb56299537200a28b9b134885e4849872",
     "b76193af29eadc6e16c66493e2a4ef8219aad79986b6d4911493741ec1e666cc",
     "941db06073e76050679d33cf1f0ec33de3e5a2cd00cb738b54c0dd90de251cdd",
     "9461b656d17db9c3f8020824b88028016d35f46d25f7888f171aa35c8cc75779",
     32},
    {"50fb68cdd31ff76b8d86b88b80124534d845cf2a835411b002b40b7b08d28278",
     "c7e0f52b886962b1edde9e75b9ecafa0d8efeffb474732a3da298c470f1d1445",
     "918e07d9bfd965f880f860830b24427d9200fcac485e973b4943e67d322c1682",
     "5e88437e7e8e91582bc440a375e93280417c94c5e38db8537963dd3750dd3e16",
     "0001000220a897b53575b4dd35fed4466e4e714bfa949eaa72e616a9c68a47b39cb7a60d2e000000000000000420918e07d9bfd965f880f860830b24427d9200fcac485e973b4943e67d322c1682205e88437e7e8e91582bc440a375e93280417c94c5e38db8537963dd3750dd3e1600",
     "b8d3d505f08d09af3a9f972fbde42eb5a5e4aa5d32ec0ea4505e9f4eeb9dc86c",
     "12b40a85172e1d6b60ccc6f999e649fb382c3dce110c16b3d7fee94ce5c40497",
     "c9706471508781f5d223b5fd64c4dcf419015eb0d920d01d0148cf3f01d060da",
     "5d590dc85dbba44d8aed8d58a48de4fbd831672a6a969c22f9fb2b9efc758bd8",
     "d8944af48586b5d3e675b3048e867749666c0b59abe4970e08a68d2c28134e68",
     "9a152300bc71d1740e46b07b1a015f3fb87136672fb91ca4acacb8171c135f91",
     "ca40a3f22bc1a7f7067970098f3df244be0ee53f3ba28226eec57d59f0015be9",
     "08300f75189cccd6eabcb9a5a3b9890cb97dad685c86fe9d96bdcc20789676a3",
     "5ac61cc2ffdbe78642bf810f1983e262dc35146234a07be6039df82e745f04a5",
     "afb46dacfaf67cb4b9a6d9b2af7b2ec93805721ce966fbe59441c9c4bd2ea208",
     "91cb6f63a85453aa5aecb36017e2881e6f1b3dd19f673d57fc6f846dbcdc69c4",
     "04196c9aa3c85fd46ff7347b2cc180f214c547851c78a6fb42d5d8640bcd20b6dcd369a7d3f5a81387ae18e99dc5338c990746fd1d24dda0aad35d55ee6f253350",
     "cc9c4b25b0bd69250b6e4f9908d1b170bf62fe8cc11ad36d33e602324ac0662a",
     "a0f762fc82d5e421d8fdf8a317b2fd463008c625bf19db7fbfa4ac778b5106a2",
     "aab9dfafc42abafb021ebce083317d461d46bb32c62d8a35b9fb169a290a0346",
     32},
};

static size_t unhex(const char *s, unsigned char *out)
{
    size_t n = 0;

    for (; s[0] && s[1]; s += 2) {
        int hi = s[0] <= '9' ? s[0] - '0' : (s[0] | 32) - 'a' + 10, lo = s[1] <= '9' ? s[1] - '0' : (s[1] | 32) - 'a' + 10;
        out[n++] = (unsigned char)(hi << 4 | lo);
    }
    return n;
}

static int same_hex(const unsigned char *got, size_t n, const char *want)
{
    unsigned char w[256];

    return unhex(want, w) == n && ct_equal(got, w, n);
}

void entry(void)
{
    unsigned char gid[64], init[32], commit[32], psk[32], th[32], cth[32], label[128], ctx[64], out[64];
    size_t gn = unhex(k_group_id, gid);

    unhex(k_initial_init, init);
    for (int i = 0; i < (int)(sizeof k_epochs / sizeof *k_epochs); i++) {
        const epoch_vector_t *v = &k_epochs[i];
        sb_t gc = {0};
        mls_epoch_t e;
        size_t ln, cn;
        int ok;
        unhex(v->commit_secret, commit);
        unhex(v->psk_secret, psk);
        unhex(v->tree_hash, th);
        unhex(v->confirmed_transcript_hash, cth);
        mls_group_context(&gc, gid, gn, (unsigned long long)i, th, cth, "", 0);
        check(same_hex((unsigned char *)gc.data, gc.len, v->group_context), "group context");
        ok = mls_key_schedule(init, commit, psk, gc.data, gc.len, &e);
        check(ok && same_hex(e.joiner, 32, v->joiner_secret) && same_hex(e.welcome, 32, v->welcome_secret), "joiner and welcome");
        check(ok && same_hex(e.init, 32, v->init_secret) && same_hex(e.sender_data, 32, v->sender_data_secret) &&
                  same_hex(e.encryption, 32, v->encryption_secret) && same_hex(e.exporter, 32, v->exporter_secret) &&
                  same_hex(e.authentication, 32, v->epoch_authenticator) && same_hex(e.external, 32, v->external_secret) &&
                  same_hex(e.confirm, 32, v->confirmation_key) && same_hex(e.membership, 32, v->membership_key) &&
                  same_hex(e.resumption, 32, v->resumption_psk),
              "epoch secrets");
        check(ok && same_hex(e.external_pub, 65, v->external_pub), "external public key");
        /* The vectors use the label's hex text itself as the label. */
        ln = (size_t)lstrlenA(v->exp_label);
        for (size_t k = 0; k < ln && k < sizeof label; k++)
            label[k] = (unsigned char)v->exp_label[k];
        cn = unhex(v->exp_context, ctx);
        check(mls_exporter(e.exporter, label, ln, ctx, cn, out, (size_t)v->exp_length) &&
                  same_hex(out, (size_t)v->exp_length, v->exp_secret),
              "exporter");
        for (int k = 0; k < 32; k++)
            init[k] = e.init[k];
        sb_free(&gc);
    }
    finish();
}
