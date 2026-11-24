<script setup>
import { ref } from 'vue'

const props = defineProps({
  platform: String,
  config: Object
})
const config = ref(props.config)
</script>

<template>
  <div id="vr" class="config-page" :data-platform="platform">
    <!-- 單行：sunshine.conf 一行一個設定，參數裡的換行會把後面吃掉 -->
    <div class="mb-3">
      <label for="vr_shortcuts" class="form-label">{{ $t('config.vr_shortcuts') }}</label>
      <input type="text" class="form-control" id="vr_shortcuts" placeholder="593600,vr -pixarkvr"
             v-model="config.vr_shortcuts" />
      <div class="form-text">{{ $t('config.vr_shortcuts_desc') }}</div>
    </div>
  </div>
</template>
