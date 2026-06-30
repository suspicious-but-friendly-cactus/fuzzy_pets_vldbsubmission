#ifndef LSH_H
#define LSH_H


template <typename InputType, typename ItemType>
class LSH {
public:
    LSH() {
    }
    virtual ~LSH() {
    }

  virtual std::vector<ItemType> hash_values(const InputType& input) {
    (void)input;
    return {};
    }
};

#endif // LSH_H
