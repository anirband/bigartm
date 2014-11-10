// Copyright 2014, Additive Regularization of Topic Models.

#include "artm/core/processor.h"

#include <stdlib.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "glog/logging.h"

#include "artm/regularizer_interface.h"
#include "artm/score_calculator_interface.h"

#include "artm/core/protobuf_helpers.h"
#include "artm/core/call_on_destruction.h"
#include "artm/core/helpers.h"
#include "artm/core/instance_schema.h"
#include "artm/core/merger.h"
#include "artm/core/topic_model.h"

#include "artm/utility/blas.h"

namespace util = artm::utility;

namespace artm {
namespace core {

Processor::Processor(ThreadSafeQueue<std::shared_ptr<const ProcessorInput> >*  processor_queue,
                     ThreadSafeQueue<std::shared_ptr<const ModelIncrement> >* merger_queue,
                     const Merger& merger,
                     const ThreadSafeHolder<InstanceSchema>& schema)
    : processor_queue_(processor_queue),
      merger_queue_(merger_queue),
      merger_(merger),
      schema_(schema),
      is_stopping(false),
      thread_() {
  // Keep this at the last action in constructor.
  // http://stackoverflow.com/questions/15751618/initialize-boost-thread-in-object-constructor
  boost::thread t(&Processor::ThreadFunction, this);
  thread_.swap(t);
}

Processor::~Processor() {
  is_stopping = true;
  if (thread_.joinable()) {
    thread_.join();
  }
}

Processor::TokenIterator::TokenIterator(
    const std::vector<Token>& token_dict,
    const std::map<ClassId, float>& class_id_to_weight,
    const TopicModel& topic_model,
    const Item& item, const std::string& field_name,
    Mode mode)
    : token_dict_(token_dict),
      class_id_to_weight_(class_id_to_weight),
      topic_model_(topic_model),
      field_(nullptr),
      token_size_(0),
      iterate_known_((mode & Mode_Known) != 0),       // NOLINT
      iterate_unknown_((mode & Mode_Unknown) != 0),   // NOLINT
      use_model_class_list_(true),
      token_index_(-1),  // handy trick for iterators
      token_(),
      token_class_weight_(0),
      id_in_model_(-1),
      id_in_batch_(-1),
      count_(0) {
  for (int field_index = 0; field_index < item.field_size(); field_index++) {
    if (item.field(field_index).name() == field_name) {
      field_ = &item.field(field_index);
    }
  }

  if (field_ == nullptr) {
    return;
  }

  token_size_ = field_->token_id_size();

  if (class_id_to_weight_.empty()) {
    use_model_class_list_ = false;
  }
}

void Processor::TokenIterator::Reset() {
  token_index_ = -1;  // handy trick for iterators

  // Reset all other data.
  // This makes it less confusing if somebody access this data after Reset() but before Next().
  token_.clear();
  id_in_model_ = -1;
  id_in_batch_ = -1;
  count_ = 0;
}

bool Processor::TokenIterator::Next() {
  if (field_ == nullptr) {
    return false;
  }

  for (;;) {
    token_index_++;  // handy trick pays you back!

    if (token_index_ >= token_size_) {
      // reached the end of the stream
      return false;
    }

    id_in_batch_ = field_->token_id(token_index_);
    token_ = token_dict_[id_in_batch_];
    count_ = field_->token_count(token_index_);
    if (count_ == 0)  // skip tokens with 0 occurrences.
      continue;

    id_in_model_ = topic_model_.token_id(token_);

    if (use_model_class_list_) {
      auto iter = class_id_to_weight_.find(token_.class_id);
      if (iter == class_id_to_weight_.end())
        continue;  // token belongs to unknown class, skip it and go to the next token.

      token_class_weight_ = iter->second;
    } else {
      token_class_weight_ = 1.0f;
    }

    if (iterate_known_ && (id_in_model_ >= 0)) {
      return true;
    }

    if (iterate_unknown_ && (id_in_model_ < 0)) {
      return true;
    }
  }

  return false;
}

TopicWeightIterator Processor::TokenIterator::GetTopicWeightIterator() const {
  return std::move(topic_model_.GetTopicWeightIterator(id_in_model()));
}

Processor::ItemProcessor::ItemProcessor(
    const TopicModel& topic_model,
    const std::vector<Token>& token_dict,
    const std::map<ClassId, float>& class_id_to_weight,
    std::shared_ptr<InstanceSchema> schema)
    : topic_model_(topic_model),
      token_dict_(token_dict),
      class_id_to_weight_(class_id_to_weight),
      schema_(schema) {}

void Processor::ItemProcessor::InferTheta(const ModelConfig& model,
                                          const Item& item,
                                          ModelIncrement* model_increment,
                                          bool update_model,
                                          float* theta) {
  int topic_size = topic_model_.topic_size();

  // find the id of token in topic_model
  std::vector<int> token_id;
  std::vector<ClassId> class_id;
  std::vector<float> token_class_weight;
  std::vector<float> token_count;
  std::vector<TopicWeightIterator> token_weights;
  std::vector<float> z_normalizer;
  int known_tokens_count = 0;
  TokenIterator iter(token_dict_,
                     class_id_to_weight_,
                     topic_model_,
                     item,
                     model.field_name(),
                     TokenIterator::Mode_Known);

  while (iter.Next()) {
    token_id.push_back(iter.id_in_batch());
    class_id.push_back(iter.token().class_id);
    token_class_weight.push_back(iter.token_class_weight());

    token_count.push_back(static_cast<float>(iter.count()));
    token_weights.push_back(iter.GetTopicWeightIterator());
    z_normalizer.push_back(0.0f);
    known_tokens_count++;
  }

  if (known_tokens_count == 0) {
    return;
  }

  int inner_iters_count = model.inner_iterations_count();
  for (int inner_iter = 0; inner_iter <= inner_iters_count; inner_iter++) {
    // 1. Find Z
    for (int token_index = 0;
          token_index < known_tokens_count;
          ++token_index) {
      float cur_z = 0.0f;
      float current_class_weight = token_class_weight[token_index];
      TopicWeightIterator topic_iter = token_weights[token_index];
      topic_iter.Reset();
      while (topic_iter.NextNonZeroTopic() < topic_size) {
        cur_z += topic_iter.Weight() * theta[topic_iter.TopicIndex()] * current_class_weight;
      }

      z_normalizer[token_index] = cur_z;
    }

    // 2. Find new theta (or store results if on the last iteration)
    std::vector<float> theta_next(topic_size);
    memset(&theta_next[0], 0, topic_size * sizeof(float));
    for (int token_index = 0;
          token_index < known_tokens_count;
          ++token_index) {
      float n_dw = token_count[token_index];
      TopicWeightIterator topic_iter = token_weights[token_index];
      float curZ = z_normalizer[token_index];

      if (curZ > 0) {
        // updating theta_next
        float current_class_weight = token_class_weight[token_index];
        topic_iter.Reset();
        while (topic_iter.NextNonZeroTopic() < topic_size) {
          theta_next[topic_iter.TopicIndex()] += current_class_weight *
            n_dw * topic_iter.Weight() * theta[topic_iter.TopicIndex()] / curZ;
        }

        if ((inner_iter == inner_iters_count) &&
            (model_increment != nullptr) &&
             update_model) {
          // Last iteration, updating final counters
          FloatArray* hat_n_wt_cur = model_increment->mutable_token_increment(
            token_id[token_index]);

          if (hat_n_wt_cur->value_size() != topic_size)
            BOOST_THROW_EXCEPTION(InternalError("hat_n_wt_cur->value_size() != topic_size"));

          topic_iter.Reset();
          while (topic_iter.NextNonZeroTopic() < topic_size) {
            float val = current_class_weight *
              n_dw * topic_iter.Weight() * theta[topic_iter.TopicIndex()] / curZ;
            hat_n_wt_cur->set_value(
              topic_iter.TopicIndex(),
              hat_n_wt_cur->value(topic_iter.TopicIndex()) + val);
          }
        }
      }
    }

    if (inner_iter == inner_iters_count) {
      // inner_iter goes from 0 to inner_iters_count inclusively.
      // The goal of this "last iteration" is to update model_increment.
      // As soon as model_increment is updated, we should exit.
      // This will save redundant calculations, and prevent
      // calling RegularizeTheta with too large inner_iter.
      break;
    }

    // 3. The following block of code makes the regularization of theta_next
    auto reg_names = model.regularizer_name();
    auto reg_tau = model.regularizer_tau();
    for (auto reg_name_iterator = reg_names.begin(); reg_name_iterator != reg_names.end();
      reg_name_iterator++) {
      auto regularizer = schema_->regularizer(reg_name_iterator->c_str());
      if (regularizer != nullptr) {
        auto tau_index = reg_name_iterator - reg_names.begin();
        double tau = reg_tau.Get(tau_index);
        bool retval = regularizer->RegularizeTheta(item, &theta_next, topic_size, inner_iter, tau);
        if (!retval) {
          LOG(ERROR) << "Problems with type or number of parameters in Theta regularizer <" <<
            reg_name_iterator->c_str() <<
            ">. On this iteration this regularizer was turned off.\n";
        }
      } else {
        LOG(ERROR) << "Theta Regularizer with name <" << reg_name_iterator->c_str() <<
          "> does not exist.";
      }
    }

    // Normalize theta_next.
    for (int i = 0; i < static_cast<int>(theta_next.size()); ++i) {
      if (theta_next[i] < 0) {
        theta_next[i] = 0;
      }
    }

    float sum = 0.0f;
    for (int topic_index = 0; topic_index < topic_size; ++topic_index)
      sum += theta_next[topic_index];

    for (int topic_index = 0; topic_index < topic_size; ++topic_index) {
      theta[topic_index] = (sum > 0) ? (theta_next[topic_index] / sum) : 0.0f;
    }
  }
}

Processor::StreamIterator::StreamIterator(const ProcessorInput& processor_input)
    : items_count_(processor_input.batch().item_size()),
      item_index_(-1),  // // handy trick for iterators
      stream_flags_(nullptr),
      processor_input_(processor_input) {
}

const Item* Processor::StreamIterator::Next() {
  for (;;) {
    item_index_++;  // handy trick pays you back!

    if (item_index_ >= items_count_) {
      // reached the end of the stream
      break;
    }

    if (!stream_flags_ || stream_flags_->value(item_index_)) {
      // found item that is included in the stream
      break;
    }
  }

  return Current();
}

const Item* Processor::StreamIterator::Current() const {
  if (item_index_ >= items_count_)
    return nullptr;

  return &(processor_input_.batch().item(item_index_));
}

bool Processor::StreamIterator::InStream(const std::string& stream_name) {
  if (item_index_ >= items_count_)
    return false;

  int index_of_stream = repeated_field_index_of(processor_input_.stream_name(), stream_name);
  if (index_of_stream == -1) {
    return true;
  }

  return processor_input_.stream_mask(index_of_stream).value(item_index_);
}

bool Processor::StreamIterator::InStream(int stream_index) {
  if (stream_index == -1)
    return true;

  assert(stream_index >= 0 && stream_index < processor_input_.stream_name_size());

  if (item_index_ >= items_count_)
    return false;

  return processor_input_.stream_mask(stream_index).value(item_index_);
}

static void PrepareTokenDictionary(const ModelConfig& model,
                                   const Batch& batch,
                                   std::vector<Token>* token_dict,
                                   std::map<ClassId, float>* class_id_to_weight) {
  // move data into map to increase lookup efficiency
  if (model.class_id_size() != 0) {
    for (int i = 0; i < model.class_id_size(); ++i) {
      class_id_to_weight->insert(std::make_pair(model.class_id(i), model.class_weight(i)));
    }
  }

  for (int token_index = 0; token_index < batch.token_size(); ++token_index) {
    token_dict->push_back(Token(batch.class_id(token_index), batch.token(token_index)));
  }
}

void Processor::FindThetaMatrix(const Batch& batch, const ModelName& model_name,
                                ThetaMatrix* theta_matrix) {
  std::shared_ptr<const TopicModel> topic_model = merger_.GetLatestTopicModel(model_name);
  if (topic_model == nullptr)
    BOOST_THROW_EXCEPTION(ArgumentOutOfRangeException("Unable to find topic model", model_name));

  std::shared_ptr<InstanceSchema> schema = schema_.get();
  const ModelConfig& model = schema_.get()->model_config(model_name);

  int topic_size = topic_model->topic_size();

  std::vector<Token> token_dict;
  std::map<ClassId, float> class_id_to_weight;
  PrepareTokenDictionary(model, batch, &token_dict, &class_id_to_weight);

  ItemProcessor item_processor(*topic_model, token_dict, class_id_to_weight, schema_.get());
  for (int item_index = 0; item_index < batch.item_size(); ++item_index) {
    const Item& item = batch.item(item_index);

    // Initialize theta
    std::vector<float> theta(topic_size);
    for (int topic_index = 0; topic_index < topic_size; ++topic_index)
      theta[topic_index] = ThreadSafeRandom::singleton().GenerateFloat();

    item_processor.InferTheta(model, item, nullptr, false, &theta[0]);

    theta_matrix->add_item_id(item.id());
    FloatArray* item_weights = theta_matrix->add_item_weights();
    for (int topic_index = 0; topic_index < topic_size; ++topic_index)
      item_weights->add_value(theta[topic_index]);
  }
}

// Create and fill Theta matrix for matrix calculations during batch processing
Matrix<float> InitializeTheta(const ProcessorInput& part, const ModelConfig& model_config, bool sparse) {
  int topic_size = model_config.topics_count();
  
  // find cache
  const DataLoaderCacheEntry* cache = nullptr;
  for (int i = 0; i < part.cached_theta_size(); ++i) {
    if ((part.cached_theta(i).batch_uuid() == part.batch_uuid()) &&
      (part.cached_theta(i).model_name() == model_config.name())) {
      cache = &part.cached_theta(i);
    }
  }

  Matrix<float> Theta;
  if (sparse) {
    Theta = Matrix<float>(topic_size, part.batch().item_size(), false);
  }
  else {
    Theta = Matrix<float>(topic_size, part.batch().item_size());
  }
  Theta.InitializeZeros();

  for (int item_index = 0; item_index < part.batch().item_size(); ++item_index) {
    int index_of_item = -1;
    if ((cache != nullptr) && model_config.reuse_theta()) {
      index_of_item = repeated_field_index_of(cache->item_id(),
        part.batch().item(item_index).id());
    }

    if ((index_of_item != -1) && model_config.reuse_theta()) {
      const FloatArray& old_thetas = cache->theta(index_of_item);
      for (int topic_index = 0; topic_index < topic_size; ++topic_index) {
        Theta(topic_index, item_index) = old_thetas.value(topic_index);
      }
    }
    else {
      for (int iTopic = 0; iTopic < topic_size; ++iTopic) {
        Theta(iTopic, item_index) = ThreadSafeRandom::singleton().GenerateFloat();
      }
    }
  }

  return Theta;
}

std::shared_ptr<ModelIncrement> InitializeModelIncrement(const ProcessorInput& part, const ModelConfig& model_config, const ::artm::core::TopicModel& topic_model) {
  std::shared_ptr<ModelIncrement> model_increment = std::make_shared<ModelIncrement>();
  model_increment->add_batch_uuid(part.batch_uuid());

  const Batch& batch = part.batch();
  int topic_size = model_config.topics_count();

  // process part and store result in merger queue
  model_increment->set_model_name(model_config.name());
  model_increment->set_topics_count(topic_size);
  for (int token_index = 0; token_index < part.batch().token_size(); ++token_index) {
    Token token = Token(batch.class_id(token_index), batch.token(token_index));
    model_increment->add_token(token.keyword);
    model_increment->add_class_id(token.class_id);
    FloatArray* counters = model_increment->add_token_increment();

    if (topic_model.has_token(token)) {
      model_increment->add_operation_type(ModelIncrement_OperationType_IncrementValue);
      for (int topic_index = 0; topic_index < topic_size; ++topic_index) {
        counters->add_value(0.0f);
      }
    }
    else {
      model_increment->add_operation_type(ModelIncrement_OperationType_CreateIfNotExist);
    }
  }

  return model_increment;
}

std::shared_ptr<Matrix<float>> InitializePhi(const ProcessorInput& part, const ModelConfig& model_config, const ::artm::core::TopicModel& topic_model) {
  bool phi_is_empty = true;

  const Batch& batch = part.batch();

  std::vector<Token> token_dict;
  std::map<ClassId, float> class_id_to_weight;
  PrepareTokenDictionary(model_config, batch, &token_dict, &class_id_to_weight);

  int topic_size = topic_model.topic_size();
  auto Phi = std::make_shared<Matrix<float>>(batch.token_size(), topic_size);
  for (int token_index = 0; token_index < batch.token_size(); ++token_index) {
    Token token = Token(batch.class_id(token_index), batch.token(token_index));

    if (topic_model.has_token(token)) {
      phi_is_empty = false;
      auto topic_iter = topic_model.GetTopicWeightIterator(token);
      for (int topic_index = 0; topic_index < topic_size; ++topic_index) {
        float class_weight = 1.0;
        auto class_id_iter = class_id_to_weight.find(token.class_id);
        if (class_id_iter != class_id_to_weight.end()) {
          class_weight = class_id_iter->second;
        }
        float value = topic_iter[topic_index] * class_weight;
        if (value < 1e-16) {
          // Reset small values to 0.0 to avoid performance hit.
          // http://en.wikipedia.org/wiki/Denormal_number#Performance_issues
          // http://stackoverflow.com/questions/13964606/inconsistent-multiplication-performance-with-floats
          value = 0.0f;
        }
        (*Phi)(token_index, topic_index) = value;
      }
    }
  }

  if (phi_is_empty)
    return nullptr;

  return Phi;
}

void Show(Matrix<float> matrix) {
  int nrows = matrix.no_rows() > 5 ? 5 : matrix.no_rows();
  int ncols = matrix.no_columns() > 5 ? 5 : matrix.no_columns();
  for (int i = 0; i < nrows; ++i) {
    for (int j = 0; j < ncols; ++j) {
      std::cout << matrix(i, j) << " ";
    }
    std::cout << "\n";
  }
}

// ToDo: make schema argument const.
void RegularizeAndNormalizeTheta(int inner_iter, const Batch& batch, const ModelConfig& model_config, InstanceSchema* schema, Matrix<float>* Theta) {
  int topic_size = model_config.topics_count();

  // next section proceed Theta regularization
  int item_index = -1;
  std::vector<float> theta_next;
  for (const Item& item : batch.item()) {
    item_index++;
    // this loop put data from blas::matrix to std::vector. It's not efficient
    // and would be avoid after final choice of data structures and corrsponding
    // adaptation of regularization interface
    theta_next.clear();
    for (int topic_index = 0; topic_index < topic_size; ++topic_index) {
      theta_next.push_back((*Theta)(topic_index, item_index));
    }

    auto reg_names = model_config.regularizer_name();
    auto reg_tau = model_config.regularizer_tau();
    for (auto reg_name_iterator = reg_names.begin();
      reg_name_iterator != reg_names.end();
      reg_name_iterator++) {
      auto regularizer = schema->regularizer(reg_name_iterator->c_str());
      if (regularizer != nullptr) {
        auto tau_index = reg_name_iterator - reg_names.begin();
        double tau = reg_tau.Get(tau_index);

        bool retval = regularizer->RegularizeTheta(
          item, &theta_next, topic_size, inner_iter, tau);
        if (!retval) {
          LOG(ERROR) << "Problems with type or number of parameters in Theta" <<
            "regularizer <" << reg_name_iterator->c_str() <<
            ">. On this iteration this regularizer was turned off.\n";
        }
      }
      else {
        LOG(ERROR) << "Theta Regularizer with name <" << reg_name_iterator->c_str() <<
          "> does not exist.";
      }
    }

    // Normalize Theta for current item
    for (int i = 0; i < static_cast<int>(theta_next.size()); ++i) {
      if (theta_next[i] < 0) {
        theta_next[i] = 0;
      }
    }

    float sum = 0.0f;
    for (int topic_index = 0; topic_index < topic_size; ++topic_index)
      sum += theta_next[topic_index];

    for (int topic_index = 0; topic_index < topic_size; ++topic_index) {
      (*Theta)(topic_index, item_index) =
        (sum > 0) ? (theta_next[topic_index] / sum) : 0.0f;
    }
  }
}

void Processor::ThreadFunction() {
  try {
    Helpers::SetThreadName(-1, "Processor thread");
    LOG(INFO) << "Processor thread started";
    int pop_retries = 0;
    const int pop_retries_max = 20;

    util::Blas* blas = util::Blas::mkl();
    if ((blas == nullptr) || !blas->is_loaded()) {
      LOG(WARNING) << "Intel Math Kernel Library is not detected, "
          << "using built in implementation (can be slower than MKL)";
      blas = util::Blas::builtin();
    }

    for (;;) {
      if (is_stopping) {
        LOG(INFO) << "Processor thread stopped";
        break;
      }

      std::shared_ptr<const ProcessorInput> part;
      if (!processor_queue_->try_pop(&part)) {
        pop_retries++;
        LOG_IF(INFO, pop_retries == pop_retries_max) << "No data in processing queue, waiting...";

        // Sleep and check for interrupt.
        // To check for interrupt without sleep,
        // use boost::this_thread::interruption_point()
        // which also throws boost::thread_interrupted
        boost::this_thread::sleep(boost::posix_time::milliseconds(kIdleLoopFrequency));

        continue;
      }

      const Batch& batch = part->batch();

      LOG_IF(INFO, pop_retries >= pop_retries_max) <<
          "Processing queue has data, processing started";
      pop_retries = 0;

      if (batch.class_id_size() != batch.token_size())
        BOOST_THROW_EXCEPTION(InternalError(
            "batch.class_id_size() != batch.token_size()"));

      std::shared_ptr<InstanceSchema> schema = schema_.get();
      std::vector<ModelName> model_names = schema->GetModelNames();

      // create and fill n_dw matrix for matrix calculations during batch processing
      Matrix<float> n_dw;
      Matrix<float> n_d;
            
      std::vector<float> n_dw_val;
      std::vector<int> n_dw_row_ptr;
      std::vector<int> n_dw_col_ind;

      // For sparse case
      for (int item_index = 0; item_index < batch.item_size(); ++item_index) {
        n_dw_row_ptr.push_back(n_dw_val.size());
        auto current_item = batch.item(item_index);
        for (auto& field : current_item.field()) {
          for (int token_index = 0; token_index < field.token_id_size(); ++token_index) {
            int token_id = field.token_id(token_index);
            int token_count = field.token_count(token_index);
            n_dw_val.push_back(token_count);
            n_dw_col_ind.push_back(token_id);
          }
        }
      }
      n_dw_row_ptr.push_back(n_dw_val.size());
      
      // For dense case
      n_dw = Matrix<float>(batch.token_size(), batch.item_size());
      n_dw.InitializeZeros();
      n_d = Matrix<float>(1, n_dw.no_columns());
      n_d.InitializeZeros();
      for (int item_index = 0; item_index < n_dw.no_columns(); ++item_index) {
        auto current_item = batch.item(item_index);
        for (auto& field : current_item.field()) {
          for (int token_index = 0; token_index < field.token_id_size(); ++token_index) {
            int token_id = field.token_id(token_index);
            int token_count = field.token_count(token_index);
            n_dw(token_id, item_index) += token_count;
            n_d(0, item_index) += token_count;
          }
        }
      }

      std::for_each(model_names.begin(), model_names.end(), [&](ModelName model_name) {
        ModelConfig& model_config = const_cast<ModelConfig&>(schema->model_config(model_name));
        model_config.set_sparse_ndw(true);

        // do not process disabled models.
        if (!model_config.enabled()) return;  // return from lambda; goes to next step of std::for_each

        if (model_config.class_id_size() != model_config.class_weight_size())
          BOOST_THROW_EXCEPTION(InternalError(
              "model.class_id_size() != model.class_weight_size()"));

        // Find and save to the variable the index of model stream in the part->stream_name() list.
        int model_stream_index = repeated_field_index_of(part->stream_name(), model_config.stream_name());

        std::shared_ptr<const TopicModel> topic_model = merger_.GetLatestTopicModel(model_name);
        assert(topic_model.get() != nullptr);

        int topic_size = topic_model->topic_size();
        if (topic_size != model_config.topics_count())
          BOOST_THROW_EXCEPTION(InternalError(
            "Topics count mismatch between model config and physical model representation"));

        Matrix<float> Theta = InitializeTheta(*part, model_config, model_config.sparse_ndw());

        std::shared_ptr<ModelIncrement> model_increment = InitializeModelIncrement(*part, model_config, *topic_model);
        call_on_destruction c([&]() { merger_queue_->push(model_increment); });

        std::shared_ptr<Matrix<float>> Phi_ptr = InitializePhi(*part, model_config, *topic_model);
        if (Phi_ptr == nullptr) {
          LOG(INFO) << "Phi is empty, calculations for the model " + model_name +
            "would not be processed on this iteration";
          return;  // return from lambda; goes to next step of std::for_each
        }

        Matrix<float>& Phi = *Phi_ptr;
        Matrix<float> Z(Phi.no_rows(), Theta.no_columns());
        if (model_config.sparse_ndw()) {
          //Show(Theta);
          for (int inner_iter = 0; inner_iter < model_config.inner_iterations_count(); ++inner_iter) {
            Matrix<float> n_td(Theta.no_rows(), Theta.no_columns(), false);
            n_td.InitializeZeros();

            int topics_count = Phi.no_columns();
            int docs_count = Theta.no_columns();
            for (int d = 0; d < docs_count; ++d) {
              for (int i = n_dw_row_ptr[d]; i < n_dw_row_ptr[d + 1]; ++i) {
                int w = n_dw_col_ind[i];
                float p_dw_val = blas->sdot(topics_count, &Phi(w, 0), 1, &Theta(0, d), 1);
                if (p_dw_val == 0) continue;
                blas->saxpy(topics_count, n_dw_val[i] / p_dw_val, &Phi(w, 0), 1, &n_td(0, d), 1);
              }
            }

            ApplyByElement<0>(&Theta, Theta, n_td);
            RegularizeAndNormalizeTheta(inner_iter, batch, model_config, schema_.get().get(), &Theta);
          }

         // Show(Theta);
        } else {
          //Show(Theta);
          for (int inner_iter = 0; inner_iter < model_config.inner_iterations_count(); ++inner_iter) {
            blas->sgemm(util::Blas::RowMajor, util::Blas::NoTrans, util::Blas::NoTrans,
              Phi.no_rows(), Theta.no_columns(), Phi.no_columns(), 1, Phi.get_data(),
              Phi.no_columns(), Theta.get_data(), Theta.no_columns(), 0, Z.get_data(),
              Theta.no_columns());

            // Z = n_dw ./ Z
            ApplyByElement<1>(&Z, n_dw, Z);

            // Theta_new = Theta .* (Phi' * Z) ./ repmat(n_d, nTopics, 1);
            Matrix<float> prod_trans_phi_Z(Phi.no_columns(), Z.no_columns());

            blas->sgemm(util::Blas::RowMajor, util::Blas::Trans, util::Blas::NoTrans,
              Phi.no_columns(), Z.no_columns(), Phi.no_rows(), 1, Phi.get_data(),
              Phi.no_columns(), Z.get_data(), Z.no_columns(), 0,
              prod_trans_phi_Z.get_data(), Z.no_columns());

            ApplyByElement<0>(&Theta, Theta, prod_trans_phi_Z);
            RegularizeAndNormalizeTheta(inner_iter, batch, model_config, schema_.get().get(), &Theta);
          }

          blas->sgemm(util::Blas::RowMajor, util::Blas::NoTrans, util::Blas::NoTrans,
            Phi.no_rows(), Theta.no_columns(), Phi.no_columns(), 1, Phi.get_data(),
            Phi.no_columns(), Theta.get_data(), Theta.no_columns(), 0, Z.get_data(),
            Theta.no_columns());
          ApplyByElement<1>(&Z, n_dw, Z);

          //Show(Theta);
        }

        Mask stream_mask;
        Matrix<float> n_wt;

////////////////////////////////////////
////////////////////////////////////////
        if (model_config.sparse_ndw()) {
          n_wt = Matrix<float>(Phi.no_rows(), Phi.no_columns());
          n_wt.InitializeZeros();

          int tokens_count = Phi.no_rows();
          int topics_count = Phi.no_columns();
          int docs_count = Theta.no_columns();

          std::vector<float> n_wd_val(n_dw_val);
          std::vector<int> n_wd_row_ptr(n_dw_row_ptr);
          std::vector<int> n_wd_col_ind(n_dw_col_ind);

          blas->scsr2csc(docs_count, tokens_count, n_dw_val.size(),
                          &n_dw_val[0], &n_dw_row_ptr[0], &n_dw_col_ind[0],
                          &n_wd_val[0], &n_wd_col_ind[0], &n_wd_row_ptr[0]);

          // n_wt should be count for items, that have corresponding true-value in stream mask
          // from batch. Or for all items, if such mask doesn't exist
          if (model_stream_index != -1) {
            stream_mask = part->stream_mask(model_stream_index);
            for (int w = 0; w < tokens_count; ++w) {
              for (int i = n_wd_row_ptr[w]; i < n_wd_row_ptr[w + 1]; ++i) {
                int d = n_wd_col_ind[i];
                if (stream_mask.value(d) == false) continue;
                float p_wd_val = blas->sdot(topics_count, &Phi(w, 0), 1, &Theta(0, d), 1);
                if (p_wd_val == 0) continue;
                blas->saxpy(topics_count, n_wd_val[i] / p_wd_val,
                    &Theta(0, d), 1, &n_wt(w, 0), 1);
              }
            }
          } else {
            for (int w = 0; w < tokens_count; ++w) {
              for (int i = n_wd_row_ptr[w]; i < n_wd_row_ptr[w + 1]; ++i) {
                int d = n_wd_col_ind[i];
                float p_wd_val = blas->sdot(topics_count, &Phi(w, 0), 1, &Theta(0, d), 1);
                if (p_wd_val == 0) continue;
                blas->saxpy(topics_count, n_wd_val[i] / p_wd_val,
                    &Theta(0, d), 1, &n_wt(w, 0), 1);
              }
            }


          //blas->scsr2csc(docs_count, tokens_count, n_dw_val.size(),
          //                &n_dw_val[0], &n_dw_row_ptr[0], &n_dw_col_ind[0],
          //                &n_wd_val[0], &n_wd_row_ind[0], &n_wd_col_ptr[0]);

          //// n_wt should be count for items, that have corresponding true-value in stream mask
          //// from batch. Or for all items, if such mask doesn't exist
          //if (model_stream_index != -1) {
          //  stream_mask = part->stream_mask(model_stream_index);
          //  for (int d = 0; d < docs_count; ++d) {
          //    for (int i = n_wd_col_ptr[d]; i < n_wd_col_ptr[d + 1]; ++i) {
          //      int w = n_wd_row_ind[i];
          //      if (stream_mask.value(d) == false) continue;
          //      float p_wd_val = blas->sdot(topics_count, &Phi(w, 0), 1, &Theta(0, d), 1);
          //      if (p_wd_val == 0) continue;
          //      blas->saxpy(topics_count, n_wd_val[i] / p_wd_val,
          //          &Theta(0, d), 1, &n_wt(w, 0), 1);
          //    }
          //  }
          //} else {
          //  for (int d = 0; d < docs_count; ++d) {
          //    for (int i = n_wd_col_ptr[d]; i < n_wd_col_ptr[d + 1]; ++i) {
          //      int w = n_wd_row_ind[i];
          //      float p_wd_val = blas->sdot(topics_count, &Phi(w, 0), 1, &Theta(0, d), 1);
          //      if (p_wd_val == 0) continue;
          //      blas->saxpy(topics_count, n_wd_val[i] / p_wd_val,
          //          &Theta(0, d), 1, &n_wt(w, 0), 1);
          //    }
          //  }
          //}


          }

          ApplyByElement<0>(&n_wt, n_wt, Phi);
          Show(n_wt);
          
////////////////////////////////////////
////////////////////////////////////////


        } else {
          if (model_stream_index != -1) {
            stream_mask = part->stream_mask(model_stream_index);

            // delete columns according to bool mask
            int true_value_count = 0;
            for (int i = 0; i < stream_mask.value_size(); ++i) {
              if (stream_mask.value(i) == true) true_value_count++;
            }

            Matrix<float> masked_Z(Z.no_rows(), true_value_count);
            Matrix<float> masked_Theta(Theta.no_rows(), true_value_count);
            int real_index = 0;
            for (int i = 0; i < stream_mask.value_size(); ++i) {
              if (stream_mask.value(i) == true) {
                for (int j = 0; j < Z.no_rows(); ++j) {
                  masked_Z(j, real_index) = Z(j, i);
                }
                for (int j = 0; j < Theta.no_rows(); ++j) {
                  masked_Theta(j, real_index) = Theta(j, i);
                }
                real_index++;
              }
            }
            Matrix<float> prod_Z_Theta(masked_Z.no_rows(), masked_Theta.no_rows());

            blas->sgemm(util::Blas::RowMajor, util::Blas::NoTrans, util::Blas::Trans,
              masked_Z.no_rows(), masked_Theta.no_rows(), masked_Z.no_columns(), 1,
              masked_Z.get_data(), masked_Z.no_columns(), masked_Theta.get_data(),
              masked_Theta.no_columns(), 0, prod_Z_Theta.get_data(), masked_Theta.no_rows());

            n_wt = Matrix<float>(Phi.no_rows(), Phi.no_columns());
            ApplyByElement<0>(&n_wt, prod_Z_Theta, Phi);
          } else {
            Matrix<float> prod_Z_Theta(Z.no_rows(), Theta.no_rows());
            blas->sgemm(util::Blas::RowMajor, util::Blas::NoTrans, util::Blas::Trans,
              Z.no_rows(), Theta.no_rows(), Z.no_columns(), 1, Z.get_data(),
              Z.no_columns(), Theta.get_data(), Theta.no_columns(), 0,
              prod_Z_Theta.get_data(), Theta.no_rows());

            n_wt = Matrix<float>(Phi.no_rows(), Phi.no_columns());
            ApplyByElement<0>(&n_wt, prod_Z_Theta, Phi);
          }

          Show(n_wt);
        }

        for (int token_index = 0; token_index < n_wt.no_rows(); ++token_index) {
          FloatArray* hat_n_wt_cur = model_increment->mutable_token_increment(token_index);

          if (hat_n_wt_cur->value_size() != topic_size)
            BOOST_THROW_EXCEPTION(InternalError("hat_n_wt_cur->value_size() != topic_size"));

          if (model_increment->operation_type(token_index) ==
              ModelIncrement_OperationType_IncrementValue) {
            for (int topic_index = 0; topic_index < topic_size; ++topic_index) {
              float value = n_wt(token_index, topic_index);
              hat_n_wt_cur->set_value(topic_index, value);
            }
          }
        }

        for (int item_index = 0; item_index < batch.item_size(); ++item_index) {
          // Update theta cache
          model_increment->add_item_id(batch.item(item_index).id());
          FloatArray* cached_theta = model_increment->add_theta();
          for (int topic_index = 0; topic_index < topic_size; ++topic_index) {
            cached_theta->add_value(Theta(topic_index, item_index));
          }
        }

        std::map<ScoreName, std::shared_ptr<Score>> score_container;
        for (int score_index = 0; score_index < model_config.score_name_size(); ++score_index) {
          const ScoreName& score_name = model_config.score_name(score_index);
          auto score_calc = schema->score_calculator(score_name);
          if (score_calc == nullptr) {
            LOG(ERROR) << "Unable to find score calculator '" << score_name << "', referenced by "
              << "model " << model_config.name() << ".";
            continue;
          }

          if (!score_calc->is_cumulative())
            continue;  // skip all non-cumulative scores

          score_container.insert(std::make_pair(score_name, score_calc->CreateScore()));
        }

        std::vector<Token> token_dict;
        std::map<ClassId, float> class_id_to_weight;
        PrepareTokenDictionary(model_config, batch, &token_dict, &class_id_to_weight);
        StreamIterator iter(*part);
        while (iter.Next() != nullptr) {
          const Item* item = iter.Current();

          // Calculate all requested scores (such as perplexity)
          for (auto score_iter = score_container.begin();
               score_iter != score_container.end();
               ++score_iter) {
            const ScoreName& score_name = score_iter->first;
            std::shared_ptr<Score> score = score_iter->second;
            auto score_calc = schema->score_calculator(score_name);

            if (!iter.InStream(score_calc->stream_name())) continue;

            int item_index = iter.item_index();
            std::vector<float> theta;
            for (int topic_index = 0; topic_index < topic_size; ++topic_index) {
              theta.push_back(Theta(topic_index, item_index));
            }
            score_calc->AppendScore(*item, token_dict, *topic_model, theta, score.get());
          }
        }

        for (auto score_iter = score_container.begin();
             score_iter != score_container.end();
             ++score_iter) {
          model_increment->add_score_name(score_iter->first);
          model_increment->add_score(score_iter->second->SerializeAsString());
        }
      });

      // Wait until merger queue has space for a new element
      int merger_queue_max_size = schema_.get()->config().merger_queue_max_size();

      int push_retries = 0;
      const int push_retries_max = 50;

      for (;;) {
        if (merger_queue_->size() < merger_queue_max_size)
          break;

        push_retries++;
        LOG_IF(WARNING, push_retries == push_retries_max) << "Merger queue is full, waiting...";
        boost::this_thread::sleep(boost::posix_time::milliseconds(kIdleLoopFrequency));
      }

      LOG_IF(WARNING, push_retries >= push_retries_max) << "Merger queue is healthy again";

      // Here call_in_destruction will enqueue processor output into the merger queue.
    }
  }
  catch(boost::thread_interrupted&) {
    LOG(WARNING) << "thread_interrupted exception in Processor::ThreadFunction() function";
    return;
  } catch(...) {
    LOG(FATAL) << "Fatal exception in Processor::ThreadFunction() function";
    throw;
  }
}

template<int operation>
void ApplyByElement(Matrix<float>* result_matrix,
                    const Matrix<float>& first_matrix,
                    const Matrix<float>& second_matrix) {
  int height = first_matrix.no_rows();
  int width = first_matrix.no_columns();

  assert(height == second_matrix.no_rows());
  assert(width == second_matrix.no_columns());

  float* result_data = result_matrix->get_data();
  const float* first_data = first_matrix.get_data();
  const float* second_data = second_matrix.get_data();
  int size = height * width;

  if (operation == 0) {
    for (int i = 0; i < size; ++i)
      result_data[i] = first_data[i] * second_data[i];
  }
  if (operation == 1) {
    for (int i = 0; i < size; ++i) {
      if (first_data[i] == 0 || second_data[i] == 0)
        result_data[i] = 0;
      else
        result_data[i] = first_data[i] / second_data[i];
    }
  }
  if (operation != 0 && operation != 1) {
    LOG(ERROR) << "In function ApplyByElement() in Processo::ThreadFunction() "
        << "'operation' argument was set an unsupported value\n";
  }
}

}  // namespace core
}  // namespace artm
