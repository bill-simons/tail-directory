#pragma once

// from:
// https://stackoverflow.com/questions/37907384/generic-handle-class -->
//     https://github.com/milleniumbug/wiertlo/blob/master/include/wiertlo/unique_handle.hpp -->
//        https://github.com/milleniumbug/wiertlo

// Licensed under Creative Commons Zero v1.0 Universal
//
//   The Creative Commons CC0 Public Domain Dedication waives copyright interest in a work
//   you've created and dedicates it to the world-wide public domain. Use CC0 to opt out of
//   copyright entirely and ensure your work has the widest reach. As with the Unlicense and
//   typical software licenses, CC0 disclaims warranties. CC0 is very similar to the Unlicense.
//   http://creativecommons.org/publicdomain/zero/1.0/

template<typename Policy>
class unique_handle
{
   typename Policy::handle_type h;

public:
   unique_handle(const unique_handle&) = delete;

   typename Policy::handle_type get() const
   {
      return h;
   }

   typename Policy::handle_type release()
   {
      typename Policy::handle_type temp = h;
      h = Policy::get_null();
      return temp;
   }

   explicit operator bool() const
   {
      return !Policy::is_null(h);
   }

   bool operator!() const
   {
      return !static_cast<bool>(*this);
   }

   void reset(typename Policy::handle_type new_handle)
   {
      typename Policy::handle_type old_handle = h;
      h = new_handle;
      if (!Policy::is_null(old_handle))
      {
         Policy::close(old_handle);
      }
   }

   void swap(unique_handle& other)
   {
      std::swap(this->h, other.h);
   }

   void reset()
   {
      reset(Policy::get_null());
   }

   ~unique_handle()
   {
      reset();
   }

   unique_handle& operator=(unique_handle other) noexcept
   {
      this->swap(other);
      return *this;
   }

   unique_handle(unique_handle&& other) noexcept
   {
      this->h = other.h;
      other.h = Policy::get_null();
   }

   unique_handle()
   {
      h = Policy::get_null();
   }

   unique_handle(typename Policy::handle_type handle)
   {
      h = handle;
   }
};
