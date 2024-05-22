#include <vector>
#include <string>
#include <functional>

typedef struct redisReply redisReply;
struct redisAsyncContext;
class reply;
typedef std::function<void(reply&)> reply_cb_t;
    /**
 * @brief Represent a reply received from redis server
 */
class reply
{
public:
    /**
     * @brief Define reply type
     */
    enum class type_t
    {
        STRING = 1,
        ARRAY = 2,
        INTEGER = 3,
        NIL = 4,
        STATUS = 5,
        ERROR = 6
    };

    /**
     * @brief Type of reply, other field values are dependent of this
     * @return
     */
    inline type_t type() const { return _type; }
    /**
     * @brief Returns string value if present, otherwise an empty string
     * @return
     */
    inline const std::string& str() const { return _str; }
    /**
     * @brief Returns integer value if present, otherwise 0
     * @return
     */
    inline long long integer() const { return _integer; }
    /**
     * @brief Returns a vector of sub-replies if present, otherwise an empty one
     * @return
     */
    inline const std::vector<reply>& elements() const { return _elements; }

    inline operator const std::string&() const { return _str; }

    inline operator long long() const { return _integer; }

    inline bool operator==(const std::string& rvalue) const
    {
		if (_type == type_t::STRING || _type == type_t::ERROR || _type == type_t::STATUS)
        {
            return _str == rvalue;
        }
        else
        {
            return false;
        }
     }

    inline bool operator==(const long long rvalue) const
    {
		if (_type == type_t::INTEGER)
        {
            return _integer == rvalue;
        }
        else
        {
            return false;
        }
    }
    static void* CbData(reply_cb_t cb) {
        if (cb) return new reply_cb_t(cb);
        return nullptr;
    }
    static void ReplyCB(redisAsyncContext* c, void* r, void* data);
private:
    reply(redisReply *reply);
    reply();
    type_t _type;
    std::string _str;
    long long _integer;
    std::vector<reply> _elements;
};

