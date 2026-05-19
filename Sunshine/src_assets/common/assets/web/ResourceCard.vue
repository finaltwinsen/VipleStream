<template>
    <div class="card">
        <div class="card-body">
            <h2>{{ $t('resource_card.resources') }}</h2>
            <p>{{ $t('resource_card.resources_desc') }}</p>
            <div class="d-flex flex-wrap gap-2 mt-4">
                <a class="btn btn-secondary" href="https://github.com/finaltwinsen/VipleStream" target="_blank">
                  <SimpleIcon icon="GitHub" :size="18" class="icon"></SimpleIcon>
                  GitHub
                </a>
            </div>
        </div>
    </div>
</template>

<script>
import SimpleIcon from './SimpleIcon.vue'

export default {
  components: {
    SimpleIcon,
  }
}
</script>
